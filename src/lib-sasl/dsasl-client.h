#ifndef DSASL_CLIENT_H
#define DSASL_CLIENT_H

#include "iostream-ssl.h"

struct dsasl_client_settings {
	/* authentication ID - must be set with most mechanisms */
	const char *authid;
	/* authorization ID (who to log in as, if authentication ID is a
	   master user) */
	const char *authzid;
	/* password - must be set with most mechanisms */
	const char *password;
};

enum dsasl_client_result {
	DSASL_CLIENT_RESULT_OK,
	/* The final response from server returned a failed authentication.
	   The error string contains details. */
	DSASL_CLIENT_RESULT_AUTH_FAILED,
	/* Remote server sent invalid SASL protocol input */
	DSASL_CLIENT_RESULT_ERR_PROTOCOL,
	/* Internal client error */
	DSASL_CLIENT_RESULT_ERR_INTERNAL,
};

typedef int
dsasl_client_channel_binding_callback_t(const char *type, void *context,
					const buffer_t **data_r,
					const char **error_r);

/* PLAIN mechanism always exists and can be accessed directly via this. */
extern const struct dsasl_client_mech dsasl_client_mech_plain;

const struct dsasl_client_mech *dsasl_client_mech_find(const char *name);
const char *dsasl_client_mech_get_name(const struct dsasl_client_mech *mech);
bool dsasl_client_mech_uses_password(const struct dsasl_client_mech *mech);

struct dsasl_client *dsasl_client_new(const struct dsasl_client_mech *mech,
				      const struct dsasl_client_settings *set);
void dsasl_client_free(struct dsasl_client **client);

/* Enable channel binding support for this client. */
void dsasl_client_enable_channel_binding(
	struct dsasl_client *client,
	enum ssl_iostream_protocol_version channel_version,
	dsasl_client_channel_binding_callback_t *callback, void *context);

/* Call for server input. */
enum dsasl_client_result
dsasl_client_input(struct dsasl_client *client,
		   const unsigned char *input, size_t input_len,
		   const char **error_r);
/* Call for getting server output. Also used to get the initial SASL response
   if supported by the protocol. */
enum dsasl_client_result
dsasl_client_output(struct dsasl_client *client,
		    const unsigned char **output_r, size_t *output_len_r,
		    const char **error_r);

/* Call for setting extra parameters for authentication, these are mechanism
   dependent. -1 = error, 0 = not found, 1 = ok
   value can be NULL. */
int dsasl_client_set_parameter(struct dsasl_client *client,
			       const char *param, const char *value,
			       const char **error_r) ATTR_NULL(3);

/* Call for getting extra result information.
  -1 = error, 0 = not found, 1 = ok */
int dsasl_client_get_result(struct dsasl_client *client,
                            const char *key, const char **value_r,
                            const char **error_r);

void dsasl_clients_init(void);
void dsasl_clients_deinit(void);

#endif
