/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "dsasl-client-private.h"

struct plain_dsasl_client {
	struct dsasl_client client;
	bool output_sent;
};

static enum dsasl_client_result
mech_plain_input(struct dsasl_client *_client,
		 const unsigned char *input ATTR_UNUSED, size_t input_len,
		 const char **error_r)
{
	struct plain_dsasl_client *client =
		(struct plain_dsasl_client *)_client;

	if (!client->output_sent) {
		if (input_len > 0) {
			*error_r = "Server sent non-empty initial response";
			return DSASL_CLIENT_RESULT_ERR_PROTOCOL;
		}
	} else {
		*error_r = "Server didn't finish authentication";
		return DSASL_CLIENT_RESULT_ERR_PROTOCOL;
	}
	return DSASL_CLIENT_RESULT_OK;
}

static enum dsasl_client_result
mech_plain_output(struct dsasl_client *_client,
		  const unsigned char **output_r, size_t *output_len_r,
		  const char **error_r)
{
	struct plain_dsasl_client *client =
		(struct plain_dsasl_client *)_client;
	string_t *str;

	if (_client->set.authid == NULL) {
		*error_r = "authid not set";
		return DSASL_CLIENT_RESULT_ERR_INTERNAL;
	}
	if (_client->password == NULL) {
		*error_r = "password not set";
		return DSASL_CLIENT_RESULT_ERR_INTERNAL;
	}

	str = str_new(_client->pool, 64);
	if (_client->set.authzid != NULL)
		str_append(str, _client->set.authzid);
	str_append_c(str, '\0');
	str_append(str, _client->set.authid);
	str_append_c(str, '\0');
	str_append(str, _client->password);

	*output_r = str_data(str);
	*output_len_r = str_len(str);
	client->output_sent = TRUE;
	return 0;
}

const struct dsasl_client_mech dsasl_client_mech_plain = {
	.name = "PLAIN",
	.struct_size = sizeof(struct plain_dsasl_client),

	.input = mech_plain_input,
	.output = mech_plain_output
};
