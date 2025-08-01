/* Copyright (c) 2004-2018 Dovecot authors, see the included COPYING file */

#include "login-common.h"
#include "connection.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "iostream.h"
#include "istream-multiplex.h"
#include "iostream-proxy.h"
#include "iostream-rawlog.h"
#include "iostream-ssl.h"
#include "llist.h"
#include "array.h"
#include "base64.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "time-util.h"
#include "settings.h"
#include "master-service.h"
#include "dsasl-client.h"
#include "client-common.h"
#include "login-proxy-state.h"
#include "login-proxy.h"


#define MAX_PROXY_INPUT_SIZE 4096
#define PROXY_MAX_OUTBUF_SIZE 1024
#define LOGIN_PROXY_DIE_IDLE_SECS 2
#define LOGIN_PROXY_KILL_PREFIX "Disconnected by proxy: "
#define KILLED_BY_ADMIN_REASON "Kicked by admin"
#define KILLED_BY_SHUTDOWN_REASON "Process shutting down"
#define LOGIN_PROXY_SIDE_SELF "proxy"
/* Wait this long before retrying on reconnect */
#define PROXY_CONNECT_RETRY_MSECS 1000
/* Don't even try to reconnect if proxying will timeout in less than this. */
#define PROXY_CONNECT_RETRY_MIN_MSECS (PROXY_CONNECT_RETRY_MSECS + 100)
#define PROXY_DISCONNECT_INTERVAL_MSECS 100
/* How many times the same ip:port can be connected to before proxying decides
   that it's a loop and fails. The first time isn't necessarily a loop, just
   a reversed dynamic decision that it was actually the proper destination. */
#define PROXY_REDIRECT_LOOP_MIN_COUNT 2

#define LOGIN_PROXY_SIDE_CLIENT IOSTREAM_PROXY_SIDE_LEFT
#define LOGIN_PROXY_SIDE_SERVER IOSTREAM_PROXY_SIDE_RIGHT

enum login_proxy_free_flags {
	LOGIN_PROXY_FREE_FLAG_DELAYED = BIT(0)
};

struct login_proxy_redirect {
	struct ip_addr ip;
	in_port_t port;
	unsigned int count;
};

struct login_proxy {
	struct login_proxy *prev, *next;
	/* Linked list of the proxies with the same virtual_user within
	   login_proxies_hash. This is set only after the proxy is detached. */
	struct login_proxy *user_prev, *user_next;

	struct client *client;
	struct event *event;
	int server_fd;
	struct io *client_wait_io, *server_io, *side_channel_io;
	struct istream *client_input, *server_input;
	struct ostream *client_output, *server_output;
	struct istream *multiplex_input, *multiplex_orig_input;
	struct istream *side_channel_input;
	struct iostream_proxy *iostream_proxy;
	struct ssl_iostream *server_ssl_iostream;
	guid_128_t anvil_conn_guid;
	uoff_t client_output_orig_offset;

	struct timeval created;
	struct timeout *to, *to_notify;
	struct login_proxy_record *state_rec;

	struct ip_addr ip, source_ip;
	char *host;
	in_port_t port;
	ARRAY(struct login_proxy_redirect) redirect_path;
	unsigned int connect_timeout_msecs;
	unsigned int notify_refresh_secs;
	unsigned int host_immediate_failure_after_secs;
	unsigned int reconnect_count;
	enum auth_proxy_ssl_flags ssl_flags;
	char *rawlog_dir;

	login_proxy_input_callback_t *input_callback;
	login_proxy_side_channel_input_callback_t *side_callback;
	login_proxy_failure_callback_t *failure_callback;
	login_proxy_redirect_callback_t *redirect_callback;

	bool connected:1;
	bool detached:1;
	bool destroying:1;
	bool delayed_disconnect:1;
	bool disable_reconnect:1;
	bool anvil_connect_sent:1;
	bool num_waiting_connections_updated:1;
};

static struct login_proxy_state *proxy_state;
static struct login_proxy *login_proxies = NULL;
static HASH_TABLE(char *, struct login_proxy *) login_proxies_hash;
static struct login_proxy *login_proxies_pending = NULL;
static struct login_proxy *login_proxies_disconnecting = NULL;
static unsigned int detached_login_proxies_count = 0;

static int login_proxy_connect(struct login_proxy *proxy);
static void login_proxy_disconnect(struct login_proxy *proxy);
static void login_proxy_free_final(struct login_proxy *proxy);
static void login_proxy_iostream_start(struct login_proxy *proxy);

static void ATTR_NULL(2)
login_proxy_free_full(struct login_proxy **_proxy, const char *log_msg,
		      const char *disconnect_side,
		      const char *disconnect_reason,
		      enum login_proxy_free_flags flags);

static struct timeval
proxy_last_io_timeval(struct login_proxy *proxy)
{
	struct timeval max_tv, tv1, tv2, tv3, tv4;

	i_stream_get_last_read_time(proxy->client_input, &tv1);
	i_stream_get_last_read_time(proxy->server_input, &tv2);
	o_stream_get_last_write_time(proxy->client_output, &tv3);
	o_stream_get_last_write_time(proxy->server_output, &tv4);

	max_tv = timeval_cmp(&tv3, &tv4) > 0 ? tv3 : tv4;
	max_tv = timeval_cmp(&max_tv, &tv2) > 0 ? max_tv : tv2;
	max_tv = timeval_cmp(&max_tv, &tv1) > 0 ? max_tv : tv1;

	return max_tv;
}

static time_t proxy_last_io(struct login_proxy *proxy)
{
	return proxy_last_io_timeval(proxy).tv_sec;
}

static void login_proxy_free_errstr(struct login_proxy **_proxy,
				    const char *errstr, bool server)
{
	struct login_proxy *proxy = *_proxy;
	string_t *log_msg = t_str_new(128);
	const char *disconnect_side = server ? "server" : "client";

	str_printfa(log_msg, "Disconnected by %s", disconnect_side);
	if (errstr[0] != '\0')
		str_printfa(log_msg, ": %s", errstr);

	str_printfa(log_msg, " (%ds idle, in=%"PRIuUOFF_T", out=%"PRIuUOFF_T,
		    (int)(ioloop_time - proxy_last_io(proxy)),
		    proxy->server_output->offset, proxy->client_output->offset);
	if (o_stream_get_buffer_used_size(proxy->client_output) > 0) {
		str_printfa(log_msg, "+%zu",
			    o_stream_get_buffer_used_size(proxy->client_output));
	}
	if (iostream_proxy_is_waiting_output(proxy->iostream_proxy,
					     LOGIN_PROXY_SIDE_SERVER))
		str_append(log_msg, ", client output blocked");
	if (iostream_proxy_is_waiting_output(proxy->iostream_proxy,
					     LOGIN_PROXY_SIDE_CLIENT))
		str_append(log_msg, ", server output blocked");

	str_append_c(log_msg, ')');
	login_proxy_free_full(_proxy, str_c(log_msg), errstr, disconnect_side,
			      server ? LOGIN_PROXY_FREE_FLAG_DELAYED : 0);
}

static void proxy_client_disconnected_input(struct login_proxy *proxy)
{
	/* we're already disconnected from server. either wait for
	   disconnection timeout or for client to disconnect itself. */
	if (i_stream_read(proxy->client_input) < 0)
		login_proxy_free_final(proxy);
	else {
		i_stream_skip(proxy->client_input,
			      i_stream_get_data_size(proxy->client_input));
	}
}

static void proxy_prelogin_input(struct login_proxy *proxy)
{
	proxy->input_callback(proxy->client);
}

static void proxy_side_channel_input(struct login_proxy *proxy)
{
	char *line;

	switch (i_stream_read(proxy->side_channel_input)) {
	case 0:
		return;
	case -2:
		i_unreached();
	case -1:
		/* Let the main channel deal with the disconnection */
		io_remove(&proxy->side_channel_io);
		return;
	default:
		break;
	}

	if (proxy->client->destroyed) {
		i_assert(proxy->client->login_proxy == NULL);
		proxy->client->login_proxy = proxy;
	}
	while ((line = i_stream_next_line(proxy->side_channel_input)) != NULL) {
		const char *error;
		const char *const *args = t_strsplit_tabescaped_inplace(line);
		if (args[0] == NULL)
			e_error(proxy->event, "Side channel input is invalid: Empty line");
		else if (proxy->side_callback == NULL)
			e_error(proxy->event, "Side channel input is unsupported: %s", line);
		else if (proxy->side_callback(proxy->client, args, &error) < 0) {
			e_error(proxy->event, "Side channel input: %s: %s", args[0], error);
			login_proxy_disconnect(proxy);
			break;
		}
	}
	if (proxy->client->destroyed)
		proxy->client->login_proxy = NULL;
}

static void proxy_plain_connected(struct login_proxy *proxy)
{
	proxy->server_input =
		i_stream_create_fd(proxy->server_fd, MAX_PROXY_INPUT_SIZE);
	proxy->server_output =
		o_stream_create_fd(proxy->server_fd, SIZE_MAX);
	o_stream_set_no_error_handling(proxy->server_output, TRUE);

	proxy->server_io =
		io_add(proxy->server_fd, IO_READ, proxy_prelogin_input, proxy);

	if (proxy->rawlog_dir != NULL) {
		if (iostream_rawlog_create(proxy->rawlog_dir,
					   &proxy->server_input,
					   &proxy->server_output) < 0)
			i_free(proxy->rawlog_dir);
	}
}

static void proxy_fail_connect(struct login_proxy *proxy)
{
	i_assert(!proxy->num_waiting_connections_updated);

	if (timeval_cmp(&proxy->created, &proxy->state_rec->last_success) < 0) {
		/* there was a successful connection done since we started
		   connecting. perhaps this is just a temporary one-off
		   failure. */
	} else {
		proxy->state_rec->last_failure = ioloop_timeval;
	}
	i_assert(proxy->state_rec->num_waiting_connections > 0);
	proxy->state_rec->num_waiting_connections--;
	proxy->num_waiting_connections_updated = TRUE;
}

void login_proxy_append_success_log_info(struct login_proxy *proxy,
					 string_t *str)
{
	long long msecs = timeval_diff_msecs(&ioloop_timeval, &proxy->created);
	str_printfa(str, " (%lld.%03lld secs", msecs/1000, msecs%1000);
	if (proxy->reconnect_count > 0)
		str_printfa(str, ", %u reconnects", proxy->reconnect_count);
	str_append_c(str, ')');
}

static void
proxy_connect_error_append(struct login_proxy *proxy, string_t *str)
{
	struct ip_addr local_ip;
	in_port_t local_port;

	if (!proxy->connected) {
		str_printfa(str, "connect(%s, %u) failed: %m",
			    net_ip2addr(&proxy->ip), proxy->port);
	} else {
		str_printfa(str, "Login timed out in state=%s",
			    client_proxy_get_state(proxy->client));
	}
	str_printfa(str, " (after %u secs",
		    (unsigned int)(ioloop_time - proxy->created.tv_sec));
	if (proxy->reconnect_count > 0)
		str_printfa(str, ", %u reconnects", proxy->reconnect_count);

	if (proxy->server_fd != -1 &&
	    net_getsockname(proxy->server_fd, &local_ip, &local_port) == 0) {
		str_printfa(str, ", local=%s",
			    net_ipport2str(&local_ip, local_port));
	} else if (proxy->source_ip.family != 0) {
		str_printfa(str, ", local=%s",
			    net_ip2addr(&proxy->source_ip));
	}

	str_append_c(str, ')');
}

static void
login_proxy_set_destination(struct login_proxy *proxy, const char *host,
			    const struct ip_addr *ip, in_port_t port)
{
	proxy->ip = *ip;
	i_free(proxy->host);
	proxy->host = i_strdup(host);
	proxy->port = port;
	proxy->state_rec = login_proxy_state_get(proxy_state, &proxy->ip,
						 proxy->port);

	/* Include destination ip:port also in the log prefix */
	event_set_append_log_prefix(
		proxy->event,
		t_strdup_printf("proxy(%s,%s): ",
				proxy->client->virtual_user,
				login_proxy_get_hostport(proxy)));
}

static void proxy_reconnect_timeout(struct login_proxy *proxy)
{
	timeout_remove(&proxy->to);
	(void)login_proxy_connect(proxy);
}

const char *login_proxy_get_hostport(const struct login_proxy *proxy)
{
	struct ip_addr ip;
	/* if we are connecting to ip address return that. */
	if (net_addr2ip(proxy->host, &ip) == 0 &&
	    net_ip_compare(&proxy->ip, &ip))
		return net_ipport2str(&proxy->ip, proxy->port);
	/* it's hostname, or hostip is also used */
	return t_strdup_printf("%s[%s]:%u", proxy->host,
			       net_ip2addr(&proxy->ip), proxy->port);
}

static bool proxy_try_reconnect(struct login_proxy *proxy)
{
	long long since_started_msecs, left_msecs;

	if (proxy->reconnect_count >= proxy->client->set->login_proxy_max_reconnects)
		return FALSE;
	if (proxy->disable_reconnect)
		return FALSE;

	since_started_msecs =
		timeval_diff_msecs(&ioloop_timeval, &proxy->created);
	if (since_started_msecs < 0)
		return FALSE; /* time moved backwards */
	left_msecs = (int)proxy->connect_timeout_msecs - since_started_msecs;
	if (left_msecs <= PROXY_CONNECT_RETRY_MIN_MSECS)
		return FALSE;

	login_proxy_disconnect(proxy);
	proxy->to = timeout_add(PROXY_CONNECT_RETRY_MSECS,
				proxy_reconnect_timeout, proxy);
	proxy->reconnect_count++;
	return TRUE;
}

static bool proxy_is_self(struct login_proxy *proxy,
			  const struct ip_addr *ip, in_port_t port)
{
	return net_ip_compare(&proxy->ip, ip) && proxy->port == port;
}

static struct login_proxy_redirect *
login_proxy_redirect_find(struct login_proxy *proxy,
			  const struct ip_addr *ip, in_port_t port)
{
	struct login_proxy_redirect *redirect;

	if (!array_is_created(&proxy->redirect_path))
		return NULL;

	array_foreach_modifiable(&proxy->redirect_path, redirect) {
		if (net_ip_compare(&redirect->ip, ip) && redirect->port == port)
			return redirect;
	}
	return NULL;
}

static bool proxy_connect_failed(struct login_proxy *proxy)
{
	string_t *str = t_str_new(128);

	if (!proxy->connected)
		proxy_fail_connect(proxy);
	proxy_connect_error_append(proxy, str);
	return login_proxy_failed(proxy, proxy->event,
				  LOGIN_PROXY_FAILURE_TYPE_CONNECT,
				  str_c(str));
}

static void proxy_wait_connect(struct login_proxy *proxy)
{
	errno = net_geterror(proxy->server_fd);
	if (errno != 0) {
		(void)proxy_connect_failed(proxy);
		return;
	}
	proxy->connected = TRUE;
	proxy->num_waiting_connections_updated = TRUE;
	proxy->state_rec->last_success = ioloop_timeval;
	i_assert(proxy->state_rec->num_waiting_connections > 0);
	proxy->state_rec->num_waiting_connections--;
	proxy->state_rec->num_proxying_connections++;
	proxy->state_rec->num_disconnects_since_ts = 0;

	io_remove(&proxy->server_io);
	proxy_plain_connected(proxy);

	if ((proxy->ssl_flags & AUTH_PROXY_SSL_FLAG_YES) != 0 &&
	    (proxy->ssl_flags & AUTH_PROXY_SSL_FLAG_STARTTLS) == 0) {
		if (login_proxy_starttls(proxy) < 0) {
			/* proxy is already destroyed */
		}
	}
}

static void proxy_connect_timeout(struct login_proxy *proxy)
{
	errno = ETIMEDOUT;
	(void)proxy_connect_failed(proxy);
}

static int login_proxy_connect(struct login_proxy *proxy)
{
	struct login_proxy_record *rec = proxy->state_rec;

	e_debug(proxy->event, "Connecting to remote host");

	/* this needs to be done early, since login_proxy_free() shrinks
	   num_waiting_connections. */
	proxy->num_waiting_connections_updated = FALSE;
	rec->num_waiting_connections++;

	if (proxy->client->local_name != NULL &&
	    !connection_is_valid_dns_name(proxy->client->local_name)) {
		login_proxy_failed(proxy, proxy->event,
				   LOGIN_PROXY_FAILURE_TYPE_INTERNAL,
				   "[BUG] Invalid local_name!");
		return -1;
	}

	if (proxy->client->proxy_ttl <= 1) {
		login_proxy_failed(proxy, proxy->event,
			LOGIN_PROXY_FAILURE_TYPE_REMOTE_CONFIG,
			"TTL reached zero - proxies appear to be looping?");
		return -1;
	}

	if (rec->last_success.tv_sec == 0) {
		/* first connect to this IP. don't start immediately failing
		   the check below. */
		rec->last_success.tv_sec = ioloop_timeval.tv_sec - 1;
	}
	int down_secs = 0;
	if (timeval_cmp(&rec->last_failure, &rec->last_success) > 0)
		down_secs = timeval_diff_msecs(&rec->last_failure,
					       &rec->last_success) / 1000;
	if (proxy->host_immediate_failure_after_secs != 0 &&
	    down_secs > (int)proxy->host_immediate_failure_after_secs &&
	    rec->num_waiting_connections > 1) {
		/* the server is down. fail immediately */
		proxy->disable_reconnect = TRUE;
		login_proxy_failed(proxy, proxy->event,
				   LOGIN_PROXY_FAILURE_TYPE_CONNECT,
				   t_strdup_printf("Host has been down for %d secs (last success was %"PRIdTIME_T")",
						   down_secs, rec->last_success.tv_sec));
		return -1;
	}

	proxy->server_fd = net_connect_ip(&proxy->ip, proxy->port,
					  proxy->source_ip.family == 0 ? NULL :
					  &proxy->source_ip);
	if (proxy->server_fd == -1) {
		if (!proxy_connect_failed(proxy))
			return -1;
		/* trying to reconnect later */
		return 0;
	}

	in_port_t source_port;
	if (net_getsockname(proxy->server_fd, NULL, &source_port) == 0)
		event_add_int(proxy->event, "source_port", source_port);

	proxy->server_io = io_add(proxy->server_fd, IO_WRITE,
				  proxy_wait_connect, proxy);
	if (proxy->connect_timeout_msecs != 0) {
		proxy->to = timeout_add(proxy->connect_timeout_msecs,
					proxy_connect_timeout, proxy);
	}
	return 0;
}

int login_proxy_new(struct client *client, struct event *event,
		    const struct login_proxy_settings *set,
		    login_proxy_input_callback_t *input_callback,
		    login_proxy_side_channel_input_callback_t *side_callback,
		    login_proxy_failure_callback_t *failure_callback,
		    login_proxy_redirect_callback_t *redirect_callback)
{
	struct login_proxy *proxy;

	i_assert(set->host != NULL && set->host[0] != '\0');
	i_assert(client->login_proxy == NULL);

	proxy = i_new(struct login_proxy, 1);
	proxy->client = client;
	proxy->event = event;
	proxy->server_fd = -1;
	proxy->created = ioloop_timeval;
	proxy->source_ip = set->source_ip;
	proxy->connect_timeout_msecs = set->connect_timeout_msecs;
	proxy->notify_refresh_secs = set->notify_refresh_secs;
	proxy->host_immediate_failure_after_secs =
		set->host_immediate_failure_after_secs;
	proxy->ssl_flags = set->ssl_flags;
	proxy->rawlog_dir = i_strdup_empty(set->rawlog_dir);
	login_proxy_set_destination(proxy, set->host, &set->ip, set->port);

	/* add event fields */
	event_add_ip(proxy->event, "source_ip",
		     login_proxy_get_source_host(proxy));
	event_add_ip(proxy->event, "dest_ip", &set->ip);
	event_add_int(proxy->event, "dest_port", set->port);
	event_add_str(event, "dest_host", set->host);
	event_add_str(event, "master_user", client->proxy_master_user);

	client_ref(client);
	event_ref(proxy->event);

	DLLIST_PREPEND(&login_proxies_pending, proxy);

	proxy->input_callback = input_callback;
	proxy->side_callback = side_callback;
	proxy->failure_callback = failure_callback;
	proxy->redirect_callback = redirect_callback;
	client->login_proxy = proxy;

	struct event_passthrough *e = event_create_passthrough(proxy->event)->
		set_name("proxy_session_started");
	e_debug(e->event(), "Created proxy session to remote host");

	return login_proxy_connect(proxy);
}

static void login_proxy_disconnect(struct login_proxy *proxy)
{
	timeout_remove(&proxy->to);
	timeout_remove(&proxy->to_notify);

	if (!proxy->num_waiting_connections_updated) {
		i_assert(proxy->state_rec->num_waiting_connections > 0);
		proxy->state_rec->num_waiting_connections--;
		proxy->num_waiting_connections_updated = TRUE;
	}
	if (proxy->connected) {
		i_assert(proxy->state_rec->num_proxying_connections > 0);
		proxy->state_rec->num_proxying_connections--;
	}

	iostream_proxy_unref(&proxy->iostream_proxy);
	ssl_iostream_destroy(&proxy->server_ssl_iostream);

	io_remove(&proxy->side_channel_io);
	io_remove(&proxy->server_io);
	i_stream_destroy(&proxy->multiplex_orig_input);
	proxy->multiplex_input = NULL;
	i_stream_destroy(&proxy->side_channel_input);
	i_stream_destroy(&proxy->server_input);
	o_stream_destroy(&proxy->server_output);
	if (proxy->server_fd != -1) {
		(void)shutdown(proxy->server_fd, SHUT_RDWR);
		net_disconnect(proxy->server_fd);
		proxy->server_fd = -1;
	}
	proxy->connected = FALSE;
}

static void login_proxy_detached_link(struct login_proxy *proxy)
{
	struct login_proxy *first_proxy;
	char *user;

	if (!hash_table_lookup_full(login_proxies_hash,
				    proxy->client->virtual_user,
				    &user, &first_proxy)) {
		user = i_strdup(proxy->client->virtual_user);
		hash_table_insert(login_proxies_hash, user, proxy);
	} else {
		DLLIST_PREPEND_FULL(&first_proxy, proxy, user_prev, user_next);
		hash_table_update(login_proxies_hash, user, proxy);
	}

	DLLIST_PREPEND(&login_proxies, proxy);
	detached_login_proxies_count++;
}

static void login_proxy_detached_unlink(struct login_proxy *proxy)
{
	struct login_proxy *first_proxy;
	char *user;

	i_assert(detached_login_proxies_count > 0);
	detached_login_proxies_count--;

	DLLIST_REMOVE(&login_proxies, proxy);

	if (!hash_table_lookup_full(login_proxies_hash,
				    proxy->client->virtual_user,
				    &user, &first_proxy))
		i_unreached();
	DLLIST_REMOVE_FULL(&first_proxy, proxy, user_prev, user_next);
	if (first_proxy != NULL)
		hash_table_update(login_proxies_hash, user, first_proxy);
	else {
		hash_table_remove(login_proxies_hash, user);
		i_free(user);
	}
}

static void login_proxy_free_final(struct login_proxy *proxy)
{
	i_assert(proxy->server_ssl_iostream == NULL);

	if (proxy->delayed_disconnect) {
		DLLIST_REMOVE(&login_proxies_disconnecting, proxy);

		i_assert(proxy->state_rec->num_delayed_client_disconnects > 0);
		if (--proxy->state_rec->num_delayed_client_disconnects == 0)
			proxy->state_rec->num_disconnects_since_ts = 0;
		timeout_remove(&proxy->to);
	}

	io_remove(&proxy->client_wait_io);
	i_stream_destroy(&proxy->client_input);
	o_stream_destroy(&proxy->client_output);
	client_unref(&proxy->client);
	event_unref(&proxy->event);
	array_free(&proxy->redirect_path);
	i_free(proxy->host);
	i_free(proxy->rawlog_dir);
	i_free(proxy);
}

static unsigned int login_proxy_delay_disconnect(struct login_proxy *proxy)
{
	struct login_proxy_record *rec = proxy->state_rec;
	const unsigned int max_delay =
		proxy->client->set->login_proxy_max_disconnect_delay;
	struct timeval disconnect_time_offset;
	unsigned int max_disconnects_per_sec, delay_msecs_since_ts, max_conns;
	long long delay_msecs;

	if (rec->num_disconnects_since_ts == 0) {
		rec->disconnect_timestamp = ioloop_timeval;
		/* start from a slightly random timestamp. this way all proxy
		   processes will disconnect at slightly different times to
		   spread the load. */
		timeval_add_msecs(&rec->disconnect_timestamp,
				  i_rand_limit(PROXY_DISCONNECT_INTERVAL_MSECS));
	}
	rec->num_disconnects_since_ts++;
	if (proxy->to != NULL) {
		/* we were already lazily disconnecting this */
		return 0;
	}
	if (max_delay == 0) {
		/* delaying is disabled */
		return 0;
	}
	max_conns = rec->num_proxying_connections + rec->num_disconnects_since_ts;
	max_disconnects_per_sec = (max_conns + max_delay-1) / max_delay;
	if (rec->num_disconnects_since_ts <= max_disconnects_per_sec &&
	    rec->num_delayed_client_disconnects == 0) {
		/* wait delaying until we have 1 second's worth of clients
		   disconnected */
		return 0;
	}

	/* see at which time we should be disconnecting the client.
	   do it in 100ms intervals so the timeouts are triggered together. */
	disconnect_time_offset = rec->disconnect_timestamp;
	delay_msecs_since_ts = PROXY_DISCONNECT_INTERVAL_MSECS *
		(max_delay * rec->num_disconnects_since_ts *
		 (1000/PROXY_DISCONNECT_INTERVAL_MSECS) / max_conns);
	timeval_add_msecs(&disconnect_time_offset, delay_msecs_since_ts);
	delay_msecs = timeval_diff_msecs(&disconnect_time_offset, &ioloop_timeval);
	if (delay_msecs <= 0) {
		/* we already reached the time */
		return 0;
	}

	rec->num_delayed_client_disconnects++;
	proxy->delayed_disconnect = TRUE;
	proxy->to = timeout_add(delay_msecs, login_proxy_free_final, proxy);
	DLLIST_PREPEND(&login_proxies_disconnecting, proxy);
	return delay_msecs;
}

static void ATTR_NULL(2)
login_proxy_free_full(struct login_proxy **_proxy, const char *log_msg,
		      const char *disconnect_reason,
		      const char *disconnect_side,
		      enum login_proxy_free_flags flags)
{
	struct login_proxy *proxy = *_proxy;
	struct client *client = proxy->client;
	unsigned int delay_ms = 0;
	const char *human_reason ATTR_UNUSED;
	const char *event_reason;

	*_proxy = NULL;

	if (proxy->destroying)
		return;
	proxy->destroying = TRUE;

	struct event_passthrough *e = event_create_passthrough(proxy->event)->
		add_str("disconnect_reason", disconnect_reason)->
		add_str("disconnect_side", disconnect_side)->
		set_name("proxy_session_finished");

	if (proxy->detached) {
		struct timeval proxy_tv = proxy_last_io_timeval(proxy);
		intmax_t idle_usecs = timeval_diff_usecs(&ioloop_timeval, &proxy_tv);
		i_assert(proxy->connected);
		e->add_int("idle_usecs", idle_usecs);
		e->add_int("net_in_bytes", proxy->server_output->offset);
		e->add_int("net_out_bytes", proxy->client_output->offset);
	} else {
		if (client_get_extra_disconnect_reason(client, &human_reason,
						       &event_reason))
			e->add_str("error_code", event_reason);
	}

	/* we'll disconnect server side in any case. */
	login_proxy_disconnect(proxy);

	if (proxy->detached) {
		/* detached proxy */
		i_assert(log_msg != NULL || proxy->client->destroyed);
		login_proxy_detached_unlink(proxy);

		if ((flags & LOGIN_PROXY_FREE_FLAG_DELAYED) != 0)
			delay_ms = login_proxy_delay_disconnect(proxy);

		if (delay_ms == 0)
			e_info(e->event(), "%s", log_msg);
		else {
			e_info(e->add_int("delay_ms", delay_ms)->event(),
			       "%s - disconnecting client in %ums",
			       log_msg, delay_ms);
		}

		struct master_service_anvil_session anvil_session = {
			.username = client->virtual_user,
			.service_name = master_service_get_name(master_service),
			.ip = client->ip,
		};
		if (proxy->anvil_connect_sent) {
			master_service_anvil_disconnect(master_service,
							&anvil_session,
							proxy->anvil_conn_guid);
		}
	} else {
		i_assert(proxy->client_input == NULL);
		i_assert(proxy->client_output == NULL);
		if (log_msg != NULL)
			e_debug(e->event(), "%s", log_msg);
		else
			e_debug(e->event(), "Failed to connect to remote host");

		DLLIST_REMOVE(&login_proxies_pending, proxy);
	}
	client->login_proxy = NULL;

	if (delay_ms == 0)
		login_proxy_free_final(proxy);
	else {
		i_assert(proxy->client_wait_io == NULL);
		proxy->client_wait_io = io_add_istream(proxy->client_input,
			proxy_client_disconnected_input, proxy);
	}
}

void login_proxy_free(struct login_proxy **_proxy)
{
	struct login_proxy *proxy = *_proxy;
	if (proxy == NULL)
		return;

	i_assert(!proxy->detached || proxy->client->destroyed);
	/* Note: The NULL error is never even attempted to be used here. */
	login_proxy_free_full(_proxy, NULL, "", LOGIN_PROXY_SIDE_SELF, 0);
}

bool login_proxy_failed(struct login_proxy *proxy, struct event *event,
			enum login_proxy_failure_type type, const char *reason)
{
	const char *log_prefix;
	bool try_reconnect = TRUE;
	event_add_str(event, "error", reason);

	switch (type) {
	case LOGIN_PROXY_FAILURE_TYPE_INTERNAL:
		log_prefix = "Aborting due to internal error: ";
		try_reconnect = FALSE;
		break;
	case LOGIN_PROXY_FAILURE_TYPE_INTERNAL_CONFIG:
		log_prefix = "";
		try_reconnect = FALSE;
		break;
	case LOGIN_PROXY_FAILURE_TYPE_CONNECT:
		log_prefix = "";
		break;
	case LOGIN_PROXY_FAILURE_TYPE_REMOTE_CONFIG:
		try_reconnect = FALSE;
		/* fall through */
	case LOGIN_PROXY_FAILURE_TYPE_REMOTE:
		log_prefix = "Aborting due to remote server: ";
		break;
	case LOGIN_PROXY_FAILURE_TYPE_PROTOCOL:
		log_prefix = "Remote server sent invalid input: ";
		break;
	case LOGIN_PROXY_FAILURE_TYPE_AUTH_REPLIED:
	case LOGIN_PROXY_FAILURE_TYPE_AUTH_NOT_REPLIED:
		log_prefix = "";
		try_reconnect = FALSE;
		break;
	case LOGIN_PROXY_FAILURE_TYPE_AUTH_TEMPFAIL:
		log_prefix = "";
		break;
	case LOGIN_PROXY_FAILURE_TYPE_AUTH_REDIRECT:
		proxy->redirect_callback(proxy->client, event, reason);
		/* return value doesn't matter here, because we can't be
		   coming from login_proxy_connect(). */
		return FALSE;
	default:
		i_unreached();
	}

	if (try_reconnect && proxy_try_reconnect(proxy)) {
		event_add_int(event, "reconnect_attempts", proxy->reconnect_count);
		event_set_name(event, "proxy_session_reconnecting");
		e_warning(event, "%s%s - reconnecting (attempt #%d)",
			  log_prefix, reason, proxy->reconnect_count);
		proxy->failure_callback(proxy->client, type, reason, TRUE);
		return TRUE;
	}

	if (type != LOGIN_PROXY_FAILURE_TYPE_AUTH_REPLIED &&
	    type != LOGIN_PROXY_FAILURE_TYPE_AUTH_NOT_REPLIED &&
	    type != LOGIN_PROXY_FAILURE_TYPE_AUTH_TEMPFAIL)
		e_error(event, "%s%s", log_prefix, reason);
	else if (proxy->client->set->auth_verbose)
		client_proxy_log_failure(proxy->client, reason);
	proxy->failure_callback(proxy->client, type, reason, FALSE);
	return FALSE;
}

int login_proxy_sasl_step(struct client *client, string_t *str)
{
	const unsigned char *data;
	size_t data_len;
	const char *error;

	enum dsasl_client_result sasl_res =
		dsasl_client_input(client->proxy_sasl_client,
				   str_data(str), str_len(str), &error);
	if (sasl_res == DSASL_CLIENT_RESULT_OK) {
		sasl_res = dsasl_client_output(client->proxy_sasl_client,
					       &data, &data_len, &error);
	}
	switch (sasl_res) {
	case DSASL_CLIENT_RESULT_OK:
		break;
	case DSASL_CLIENT_RESULT_AUTH_FAILED:
		login_proxy_failed(client->login_proxy,
				   login_proxy_get_event(client->login_proxy),
				   LOGIN_PROXY_FAILURE_TYPE_AUTH_NOT_REPLIED,
				   error);
		return -1;
	case DSASL_CLIENT_RESULT_ERR_PROTOCOL: {
		const char *reason = t_strdup_printf(
			"Invalid authentication data: %s", error);
		login_proxy_failed(client->login_proxy,
				   login_proxy_get_event(client->login_proxy),
				   LOGIN_PROXY_FAILURE_TYPE_PROTOCOL, reason);
		return -1;
	}
	case DSASL_CLIENT_RESULT_ERR_INTERNAL:
		login_proxy_failed(client->login_proxy,
				   login_proxy_get_event(client->login_proxy),
				   LOGIN_PROXY_FAILURE_TYPE_INTERNAL, error);
		return -1;
	}
	str_truncate(str, 0);
	base64_encode(data, data_len, str);
	return 0;
}

bool login_proxy_is_ourself(const struct client *client, const char *host,
			    const struct ip_addr *hostip,
			    in_port_t port, const char *destuser)
{
	struct ip_addr ip;

	if (port != client->local_port)
		return FALSE;

	if (hostip != NULL)
		ip = *hostip;
	else if (net_addr2ip(host, &ip) < 0)
		return FALSE;
	if (!net_ip_compare(&ip, &client->local_ip))
		return FALSE;

	return strcmp(client->virtual_user, destuser) == 0;
}

void login_proxy_redirect_finish(struct login_proxy *proxy,
				 const struct ip_addr *ip, in_port_t port)
{
	struct login_proxy_redirect *redirect;
	bool looping;

	i_assert(port != 0);

	/* If proxy destination is the socket's local IP/port, it's a definite
	   immediate loop. */
	looping = proxy_is_self(proxy, ip, port);
	if (!looping) {
		/* If the proxy destination has already been connected too
		   many times, assume it's a loop. */
		redirect = login_proxy_redirect_find(proxy, ip, port);
		looping = redirect != NULL &&
			redirect->count >= PROXY_REDIRECT_LOOP_MIN_COUNT;
	}
	if (looping) {
		const char *error = t_strdup_printf(
			"Proxying loops - already connected to %s:%d",
			net_ip2addr(ip), port);
		login_proxy_failed(proxy, proxy->event,
			LOGIN_PROXY_FAILURE_TYPE_INTERNAL_CONFIG, error);
		return;
	}
	i_assert(proxy->client->proxy_ttl > 0);
	proxy->client->proxy_ttl--;

	if (redirect == NULL) {
		/* add current ip/port to redirect path */
		if (!array_is_created(&proxy->redirect_path))
			i_array_init(&proxy->redirect_path, 2);
		redirect = array_append_space(&proxy->redirect_path);
		redirect->ip = proxy->ip;
		redirect->port = proxy->port;
	}
	redirect->count++;

	/* disconnect from current backend */
	login_proxy_disconnect(proxy);

	e_debug(proxy->event, "Redirecting to %s", net_ipport2str(ip, port));
	login_proxy_set_destination(proxy, net_ip2addr(ip), ip, port);
	(void)login_proxy_connect(proxy);
}

void login_proxy_get_redirect_path(struct login_proxy *proxy, string_t *str)
{
	const struct login_proxy_redirect *redirect;

	str_printfa(str, "%s", net_ipport2str(&proxy->ip, proxy->port));
	if (!array_is_created(&proxy->redirect_path))
		return;
	array_foreach(&proxy->redirect_path, redirect) {
		str_printfa(str, ",%s",
			    net_ipport2str(&redirect->ip, redirect->port));
	}
}

void login_proxy_replace_client_iostream_pre(struct login_proxy *proxy)
{
	struct client *client = proxy->client;

	i_assert(client->input == NULL);
	i_assert(client->output == NULL);

	iostream_proxy_unref(&proxy->iostream_proxy);
	proxy->client_output_orig_offset = proxy->client_output->offset;

	/* Temporarily move the iostreams back to client. This allows plugins
	   to hook into iostream changes even after proxying is started. */
	client->input = proxy->client_input;
	client->output = proxy->client_output;

	/* iostream_change_pre() may change iostreams */
	if (client->v.iostream_change_pre != NULL)
		client->v.iostream_change_pre(client);
	client_rawlog_deinit(client);

	proxy->client_input = client->input;
	proxy->client_output = client->output;
}

void login_proxy_replace_client_iostream_post(struct login_proxy *proxy,
					      struct istream *new_input,
					      struct ostream *new_output)
{
	struct client *client = proxy->client;

	i_assert(client->input == proxy->client_input);
	i_assert(client->output == proxy->client_output);
	i_assert(new_input != proxy->client_input);
	i_assert(new_output != proxy->client_output);

	client->input = new_input;
	client->output = new_output;

	i_stream_unref(&proxy->client_input);
	o_stream_unref(&proxy->client_output);

	if (client->v.iostream_change_post != NULL)
		client->v.iostream_change_post(client);
	client_rawlog_init(client);

	/* iostream_change_post() may have replaced the iostreams */
	proxy->client_input = client->input;
	proxy->client_output = client->output;
	/* preserve output offset so that the bytes out counter in logout
	   message doesn't get reset here */
	proxy->client_output->offset = proxy->client_output_orig_offset;

	client->input = NULL;
	client->output = NULL;

	login_proxy_iostream_start(proxy);
}

struct istream *login_proxy_get_client_istream(struct login_proxy *proxy)
{
	return proxy->client_input;
}

struct ostream *login_proxy_get_client_ostream(struct login_proxy *proxy)
{
	return proxy->client_output;
}

struct istream *login_proxy_get_server_istream(struct login_proxy *proxy)
{
	return proxy->server_input;
}

struct ostream *login_proxy_get_server_ostream(struct login_proxy *proxy)
{
	return proxy->server_output;
}

struct event *login_proxy_get_event(struct login_proxy *proxy)
{
	return proxy->event;
}

const struct ip_addr *
login_proxy_get_source_host(const struct login_proxy *proxy)
{
	return &proxy->source_ip;
}

const char *login_proxy_get_host(const struct login_proxy *proxy)
{
	return proxy->host;
}

const char *login_proxy_get_ip_str(const struct login_proxy *proxy)
{
	return net_ip2addr(&proxy->ip);
}

in_port_t login_proxy_get_port(const struct login_proxy *proxy)
{
	return proxy->port;
}

enum auth_proxy_ssl_flags
login_proxy_get_ssl_flags(const struct login_proxy *proxy)
{
	return proxy->ssl_flags;
}

unsigned int
login_proxy_get_connect_timeout_msecs(const struct login_proxy *proxy)
{
	return proxy->connect_timeout_msecs;
}

static void
login_proxy_finished(enum iostream_proxy_side side,
		     enum iostream_proxy_status status,
		     struct login_proxy *proxy)
{
	const char *errstr;
	bool server_side;

	server_side = side == LOGIN_PROXY_SIDE_SERVER;
	switch (status) {
	case IOSTREAM_PROXY_STATUS_INPUT_EOF:
		/* success */
		errstr = "";
		break;
	case IOSTREAM_PROXY_STATUS_INPUT_ERROR:
		errstr = side == LOGIN_PROXY_SIDE_CLIENT ?
			i_stream_get_error(proxy->client_input) :
			i_stream_get_error(proxy->server_input);
		break;
	case IOSTREAM_PROXY_STATUS_OTHER_SIDE_OUTPUT_ERROR:
		server_side = !server_side;
		errstr = side == LOGIN_PROXY_SIDE_CLIENT ?
			o_stream_get_error(proxy->server_output) :
			o_stream_get_error(proxy->client_output);
		break;
	default:
		i_unreached();
	}
	login_proxy_free_errstr(&proxy, errstr, server_side);
}

static void login_proxy_notify(struct login_proxy *proxy)
{
	login_proxy_state_notify(proxy_state, proxy->client->proxy_user);
}

static bool
client_get_alt_usernames(struct client *client,
			 ARRAY_TYPE(const_string) *strings)
{
	unsigned int alt_username_count =
		str_array_length(client->alt_usernames);
	if (alt_username_count == 0)
		return FALSE;

	t_array_init(strings, alt_username_count * 2 + 1);
	for (unsigned int i = 0; i < alt_username_count; i++) {
		if (client->alt_usernames[i][0] == '\0')
			continue;

		const char *field_name =
			array_idx_elem(&global_alt_usernames, i);
		array_push_back(strings, &field_name);
		array_push_back(strings, &client->alt_usernames[i]);
	}
	return TRUE;
}

static void login_proxy_iostream_start(struct login_proxy *proxy)
{
	proxy->iostream_proxy =
		iostream_proxy_create(proxy->client_input, proxy->client_output,
				      proxy->server_input, proxy->server_output);
	iostream_proxy_set_completion_callback(proxy->iostream_proxy,
					       login_proxy_finished, proxy);
	iostream_proxy_start(proxy->iostream_proxy);
}

void login_proxy_detach(struct login_proxy *proxy)
{
	struct client *client = proxy->client;

	pool_unref(&proxy->client->preproxy_pool);

	i_assert(!proxy->detached);
	i_assert(proxy->server_input != NULL);
	i_assert(proxy->server_output != NULL);

	timeout_remove(&proxy->to);
	io_remove(&proxy->server_io);

	proxy->detached = TRUE;
	proxy->client_input = client->input;
	proxy->client_output = client->output;
	client->input = NULL;
	client->output = NULL;

	if (proxy->multiplex_orig_input != NULL &&
	    client->multiplex_output == proxy->client_output) {
		/* both sides of the proxy want multiplexing and there are no
		   plugins hooking into the ostream. We can just step out of
		   the way and let the two sides multiplex directly. */
		i_stream_unref(&proxy->side_channel_input);
		i_stream_unref(&proxy->server_input);
		proxy->server_input = proxy->multiplex_orig_input;
		proxy->multiplex_input = NULL;
		proxy->multiplex_orig_input = NULL;

		o_stream_unref(&proxy->client_output);
		proxy->client_output = client->multiplex_orig_output;
		client->multiplex_output = NULL;
		client->multiplex_orig_output = NULL;
	}
	o_stream_set_max_buffer_size(proxy->client_output,
				     PROXY_MAX_OUTBUF_SIZE);

	/* from now on, just do dummy proxying */
	login_proxy_iostream_start(proxy);

	if (proxy->notify_refresh_secs != 0) {
		proxy->to_notify =
			timeout_add(proxy->notify_refresh_secs * 1000,
				    login_proxy_notify, proxy);
	}

	proxy->input_callback = NULL;
	proxy->failure_callback = NULL;

	i_assert(!proxy->anvil_connect_sent);
	struct master_service_anvil_session anvil_session = {
		.username = client->virtual_user,
		.service_name = master_service_get_name(master_service),
		.ip = client->ip,
		.dest_ip = proxy->ip,
	};
	ARRAY_TYPE(const_string) alt_usernames;
	if (client_get_alt_usernames(client, &alt_usernames)) {
		array_append_zero(&alt_usernames);
		anvil_session.alt_usernames = array_idx(&alt_usernames, 0);
	}
	if (master_service_anvil_connect(master_service, &anvil_session,
					 TRUE, proxy->anvil_conn_guid))
		proxy->anvil_connect_sent = TRUE;

	DLLIST_REMOVE(&login_proxies_pending, proxy);
	login_proxy_detached_link(proxy);

	client->login_proxy = NULL;
}

int login_proxy_starttls(struct login_proxy *proxy)
{
	const char *error;
	bool add_multiplex_istream = FALSE;

	/* NOTE: We're explicitly disabling ssl_client_ca_* settings for now
	   at least. The main problem is that we're chrooted, so we can't read
	   them at this point anyway. The second problem is that especially
	   ssl_client_ca_dir does blocking disk I/O, which could cause
	   unexpected hangs when login process handles multiple clients. */
	enum ssl_iostream_flags ssl_flags = SSL_IOSTREAM_FLAG_DISABLE_CA_FILES;
	if ((proxy->ssl_flags & AUTH_PROXY_SSL_FLAG_ANY_CERT) != 0)
		ssl_flags |= SSL_IOSTREAM_FLAG_ALLOW_INVALID_CERT;

	io_remove(&proxy->side_channel_io);
	io_remove(&proxy->server_io);

	if (proxy->multiplex_orig_input != NULL) {
		/* restart multiplexing after TLS iostreams are set up */
		i_assert(proxy->server_input == proxy->multiplex_input);
		i_stream_unref(&proxy->server_input);
		proxy->server_input = proxy->multiplex_orig_input;
		i_stream_unref(&proxy->side_channel_input);
		proxy->multiplex_input = NULL;
		proxy->multiplex_orig_input = NULL;
		add_multiplex_istream = TRUE;
	}
	const struct ssl_iostream_client_autocreate_parameters parameters = {
		.event_parent = proxy->event,
		.host = proxy->host,
		.flags = ssl_flags,
		.application_protocols = login_binary->application_protocols,
	};
	if (io_stream_autocreate_ssl_client(&parameters,
					    &proxy->server_input,
					    &proxy->server_output,
					    &proxy->server_ssl_iostream,
					    &error) < 0) {
		const char *reason = t_strdup_printf(
			"Failed to create SSL client: %s", error);
		login_proxy_failed(proxy, proxy->event,
				   LOGIN_PROXY_FAILURE_TYPE_INTERNAL, reason);
		return -1;
	}

	if (ssl_iostream_handshake(proxy->server_ssl_iostream) < 0) {
		error = ssl_iostream_get_last_error(proxy->server_ssl_iostream);
		const char *reason = t_strdup_printf(
			"Failed to start SSL handshake: %s",
			ssl_iostream_get_last_error(proxy->server_ssl_iostream));
		login_proxy_failed(proxy, proxy->event,
				   LOGIN_PROXY_FAILURE_TYPE_INTERNAL, reason);
		return -1;
	}

	proxy->server_io = io_add_istream(proxy->server_input,
					  proxy_prelogin_input, proxy);
	if (add_multiplex_istream)
		login_proxy_multiplex_input_start(proxy);
	return 0;
}

void login_proxy_multiplex_input_start(struct login_proxy *proxy)
{
	struct istream *input = i_stream_create_multiplex(proxy->server_input,
							  LOGIN_MAX_INBUF_SIZE);
	i_assert(proxy->multiplex_orig_input == NULL);
	proxy->multiplex_orig_input = proxy->server_input;
	proxy->multiplex_input = input;
	proxy->server_input = input;

	proxy->side_channel_input =
		i_stream_multiplex_add_channel(proxy->server_input, 1);
	i_assert(proxy->side_channel_io == NULL);
	proxy->side_channel_io =
		io_add_istream(proxy->side_channel_input,
			       proxy_side_channel_input, proxy);

	io_remove(&proxy->server_io);
	proxy->server_io = io_add_istream(proxy->server_input,
					  proxy_prelogin_input, proxy);
	/* caller needs to break out of the proxy_input() loop and get it
	   called again to update the istream. */
	i_stream_set_input_pending(input, TRUE);
}

static void proxy_kill_idle(struct login_proxy *proxy)
{
	login_proxy_free_full(&proxy,
		LOGIN_PROXY_KILL_PREFIX KILLED_BY_SHUTDOWN_REASON,
		KILLED_BY_SHUTDOWN_REASON,
		LOGIN_PROXY_SIDE_SELF, 0);
}

void login_proxy_kill_idle(void)
{
	struct login_proxy *proxy, *next;
	time_t now = time(NULL);
	time_t stop_timestamp = now - LOGIN_PROXY_DIE_IDLE_SECS;
	unsigned int stop_msecs;

	for (proxy = login_proxies; proxy != NULL; proxy = next) {
		next = proxy->next;
		time_t last_io = proxy_last_io(proxy);

		if (last_io <= stop_timestamp)
			proxy_kill_idle(proxy);
		else {
			i_assert(proxy->to == NULL);
			stop_msecs = (last_io - stop_timestamp) * 1000;
			proxy->to = timeout_add(stop_msecs,
						proxy_kill_idle, proxy);
		}
	}
}

unsigned int
login_proxy_kick_user_connection(const char *user, const guid_128_t conn_guid)
{
	struct login_proxy *proxy, *next;
	unsigned int count = 0;
	bool match_conn_guid = conn_guid != NULL &&
		!guid_128_is_empty(conn_guid);

	proxy = hash_table_lookup(login_proxies_hash, user);
	for (; proxy != NULL; proxy = next) T_BEGIN {
		next = proxy->user_next;

		if (!match_conn_guid ||
		    guid_128_cmp(proxy->anvil_conn_guid, conn_guid) == 0) {
			login_proxy_free_full(&proxy,
				LOGIN_PROXY_KILL_PREFIX KILLED_BY_ADMIN_REASON,
				KILLED_BY_ADMIN_REASON,
				LOGIN_PROXY_SIDE_SELF,
				LOGIN_PROXY_FREE_FLAG_DELAYED);
			count++;
		}
	} T_END;
	for (proxy = login_proxies_pending; proxy != NULL; proxy = next) T_BEGIN {
		next = proxy->next;

		if (strcmp(proxy->client->virtual_user, user) == 0 &&
		    (!match_conn_guid ||
		     guid_128_cmp(proxy->anvil_conn_guid, conn_guid) == 0)) {
			client_disconnect(proxy->client,
				LOGIN_PROXY_KILL_PREFIX KILLED_BY_ADMIN_REASON);
			client_destroy(proxy->client, NULL);
			count++;
		}
	} T_END;
	return count;
}

unsigned int login_proxies_get_detached_count(void)
{
	return detached_login_proxies_count;
}

struct client *login_proxies_get_first_detached_client(void)
{
	return login_proxies == NULL ? NULL : login_proxies->client;
}

void login_proxy_init(const char *proxy_notify_pipe_path)
{
	proxy_state = login_proxy_state_init(proxy_notify_pipe_path);
	hash_table_create(&login_proxies_hash, default_pool, 0,
			  str_hash, strcmp);
}

void login_proxy_deinit(void)
{
	struct login_proxy *proxy;

	while (login_proxies != NULL) {
		proxy = login_proxies;
		login_proxy_free_full(&proxy,
			LOGIN_PROXY_KILL_PREFIX KILLED_BY_SHUTDOWN_REASON,
			KILLED_BY_SHUTDOWN_REASON,
			LOGIN_PROXY_SIDE_SELF, 0);
	}
	i_assert(detached_login_proxies_count == 0);

	while (login_proxies_disconnecting != NULL)
		login_proxy_free_final(login_proxies_disconnecting);

	i_assert(hash_table_count(login_proxies_hash) == 0);
	hash_table_destroy(&login_proxies_hash);
	login_proxy_state_deinit(&proxy_state);
}
