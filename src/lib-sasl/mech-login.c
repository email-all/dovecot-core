/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "dsasl-client-private.h"

enum login_state {
	STATE_INIT = 0,
	STATE_USER,
	STATE_PASS
};

struct login_dsasl_client {
	struct dsasl_client client;
	enum login_state state;
};

static enum dsasl_client_result
mech_login_input(struct dsasl_client *_client,
		 const unsigned char *input ATTR_UNUSED,
		 size_t input_len ATTR_UNUSED,
		 const char **error_r)
{
	struct login_dsasl_client *client =
		(struct login_dsasl_client *)_client;

	if (client->state == STATE_PASS) {
		*error_r = "Server didn't finish authentication";
		return DSASL_CLIENT_RESULT_ERR_PROTOCOL;
	}
	client->state++;
	return DSASL_CLIENT_RESULT_OK;
}

static enum dsasl_client_result
mech_login_output(struct dsasl_client *_client,
		  const unsigned char **output_r, size_t *output_len_r,
		  const char **error_r)
{
	struct login_dsasl_client *client =
		(struct login_dsasl_client *)_client;

	if (_client->set.authid == NULL) {
		*error_r = "authid not set";
		return DSASL_CLIENT_RESULT_ERR_INTERNAL;
	}
	if (_client->password == NULL) {
		*error_r = "password not set";
		return DSASL_CLIENT_RESULT_ERR_INTERNAL;
	}

	switch (client->state) {
	case STATE_INIT:
		*output_r = uchar_empty_ptr;
		*output_len_r = 0;
		return DSASL_CLIENT_RESULT_OK;
	case STATE_USER:
		*output_r = (const unsigned char *)_client->set.authid;
		*output_len_r = strlen(_client->set.authid);
		return DSASL_CLIENT_RESULT_OK;
	case STATE_PASS:
		*output_r = (const unsigned char *)_client->set.password;
		*output_len_r = strlen(_client->set.password);
		return DSASL_CLIENT_RESULT_OK;
	}
	i_unreached();
}

const struct dsasl_client_mech dsasl_client_mech_login = {
	.name = "LOGIN",
	.struct_size = sizeof(struct login_dsasl_client),

	.input = mech_login_input,
	.output = mech_login_output
};
