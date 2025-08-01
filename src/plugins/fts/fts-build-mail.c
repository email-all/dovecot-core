/* Copyright (c) 2006-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream.h"
#include "buffer.h"
#include "str.h"
#include "rfc822-parser.h"
#include "message-address.h"
#include "message-parser.h"
#include "message-decoder.h"
#include "mail-storage.h"
#include "index-mail.h"
#include "fts-parser.h"
#include "fts-user.h"
#include "language.h"
#include "lang-tokenizer.h"
#include "lang-filter.h"
#include "lang-user.h"
#include "fts-api-private.h"
#include "fts-build-mail.h"

/* there are other characters as well, but this doesn't have to be exact */
#define IS_WORD_WHITESPACE(c) \
	((c) == ' ' || (c) == '\t' || (c) == '\n')
/* if we see a word larger than this, just go ahead and split it from
   wherever */
#define MAX_WORD_SIZE 1024

struct fts_mail_build_context {
	struct mail *mail;
	struct fts_backend_update_context *update_ctx;

	char *content_type, *content_type_params, *content_disposition;
	struct fts_parser *body_parser;

	buffer_t *word_buf, *pending_input;
	struct language_user *cur_user_lang;
};

static int fts_build_data(struct fts_mail_build_context *ctx,
			  const unsigned char *data, size_t size, bool last);

static void fts_build_parse_content_type(struct fts_mail_build_context *ctx,
					 const struct message_header_line *hdr)
{
	struct rfc822_parser_context parser;
	string_t *content_type;

	if (ctx->content_type != NULL)
		return;

	rfc822_parser_init(&parser, hdr->full_value, hdr->full_value_len, NULL);
	rfc822_skip_lwsp(&parser);

	T_BEGIN {
		content_type = t_str_new(64);
		(void)rfc822_parse_content_type(&parser, content_type);
		/* Parse the mime-type only... */
		ctx->content_type = str_lcase(i_strdup(str_c(content_type)));
		/* ... then store the remainder of the line - which may contain RFC2231
		   parameters - without parsing it because not all backends need them. In
		   the backends that need them further parsing can be implemented. */
		ctx->content_type_params = i_strdup((const char *)parser.data);
	} T_END;
	rfc822_parser_deinit(&parser);
}

static void
fts_build_parse_content_disposition(struct fts_mail_build_context *ctx,
				    const struct message_header_line *hdr)
{
	/* just pass it as-is to backend. */
	i_free(ctx->content_disposition);
	ctx->content_disposition =
		i_strndup(hdr->full_value, hdr->full_value_len);
}

static void fts_parse_mail_header(struct fts_mail_build_context *ctx,
				  const struct message_block *raw_block)
{
	const struct message_header_line *hdr = raw_block->hdr;

	if (strcasecmp(hdr->name, "Content-Type") == 0)
		fts_build_parse_content_type(ctx, hdr);
	else if (strcasecmp(hdr->name, "Content-Disposition") == 0)
		fts_build_parse_content_disposition(ctx, hdr);
}

static int
fts_build_unstructured_header(struct fts_mail_build_context *ctx,
			      const struct message_header_line *hdr)
{
	const unsigned char *data = hdr->full_value;
	unsigned char *buf = NULL;
	unsigned int i;
	int ret;

	/* @UNSAFE: if there are any NULs, replace them with spaces */
	for (i = 0; i < hdr->full_value_len; i++) {
		if (hdr->full_value[i] == '\0') {
			if (buf == NULL) {
				buf = i_memdup(hdr->full_value,
					       hdr->full_value_len);
				data = buf;
			}
			buf[i] = ' ';
		}
	}
	ret = fts_build_data(ctx, data, hdr->full_value_len, TRUE);
	i_free(buf);
	return ret;
}

static void fts_mail_build_ctx_set_lang(struct fts_mail_build_context *ctx,
					struct language_user *user_lang)
{
	i_assert(user_lang != NULL);

	ctx->cur_user_lang = user_lang;
	/* reset tokenizer between fields - just to be sure no state
	   leaks between fields (especially if previous indexing had
	   failed) */
	lang_tokenizer_reset(user_lang->index_tokenizer);
}

static void
fts_build_tokenized_hdr_update_lang(struct fts_mail_build_context *ctx,
				    const struct message_header_line *hdr)
{
	/* Headers that don't contain any human language will only be
	   translated to lowercase - no stemming or other filtering. There's
	   unfortunately no perfect way of detecting which headers contain
	   human languages, so we check with fts_header_has_language if the
	   header is something that's supposed to containing human text. */
	if (fts_header_has_language(hdr->name))
		ctx->cur_user_lang = NULL;
	else {
		fts_mail_build_ctx_set_lang(ctx,
			lang_user_get_data_lang(ctx->update_ctx->backend->ns->user));
	}
}

static int fts_build_mail_header(struct fts_mail_build_context *ctx,
				 const struct message_block *block)
{
	const struct message_header_line *hdr = block->hdr;
	struct fts_backend_build_key key;
	int ret;

	if (hdr->eoh)
		return 0;

	/* hdr->full_value is always set because we get the block from
	   message_decoder */
	i_zero(&key);
	key.uid = ctx->mail->uid;
	key.type = block->part->physical_pos == 0 ?
		FTS_BACKEND_BUILD_KEY_HDR : FTS_BACKEND_BUILD_KEY_MIME_HDR;
	key.part = block->part;
	key.hdr_name = hdr->name;

	if ((ctx->update_ctx->backend->flags &
	     FTS_BACKEND_FLAG_TOKENIZED_INPUT) != 0)
		fts_build_tokenized_hdr_update_lang(ctx, hdr);

	if (!fts_backend_update_set_build_key(ctx->update_ctx, &key))
		return 0;

	if (!message_header_is_address(hdr->name)) {
		/* regular unstructured header */
		ret = fts_build_unstructured_header(ctx, hdr);
	} else T_BEGIN {
		/* message address. normalize it to give better
		   search results. */
		struct message_address *addr;
		string_t *str;

		addr = message_address_parse(pool_datastack_create(),
					     hdr->full_value,
					     hdr->full_value_len,
					     UINT_MAX, 0);
		str = t_str_new(hdr->full_value_len);
		message_address_write(str, addr);

		ret = fts_build_data(ctx, str_data(str), str_len(str), TRUE);
	} T_END;

	if ((ctx->update_ctx->backend->flags &
	     FTS_BACKEND_FLAG_TOKENIZED_INPUT) != 0) {
		/* index the header name itself using data-language. */
		struct language_user *prev_lang = ctx->cur_user_lang;

		fts_mail_build_ctx_set_lang(ctx,
			lang_user_get_data_lang(ctx->update_ctx->backend->ns->user));
		key.hdr_name = "";
		if (fts_backend_update_set_build_key(ctx->update_ctx, &key)) {
			if (fts_build_data(ctx, (const void *)hdr->name,
					   strlen(hdr->name), TRUE) < 0)
				ret = -1;
		}
		fts_mail_build_ctx_set_lang(ctx, prev_lang);
	}
	return ret;
}

static bool
fts_build_body_begin(struct fts_mail_build_context *ctx,
		     struct message_part *part, bool *binary_body_r)
{
	struct mail_storage *storage;
	struct fts_parser_context parser_context;
	struct fts_backend_build_key key;

	i_assert(ctx->body_parser == NULL);

	*binary_body_r = FALSE;
	i_zero(&key);
	key.uid = ctx->mail->uid;
	key.part = part;

	i_zero(&parser_context);
	if (ctx->content_type != NULL) {
		parser_context.content_type = ctx->content_type;
		parser_context.content_type_params = ctx->content_type_params;
	} else {
		parser_context.content_type = "text/plain";
		parser_context.content_type_params = "";
	}

	if (str_begins_with(parser_context.content_type, "multipart/")) {
		/* multiparts are never indexed, only their contents */
		return FALSE;
	}
	storage = mailbox_get_storage(ctx->mail->box);
	parser_context.user = mail_storage_get_user(storage);
	parser_context.content_disposition = ctx->content_disposition;
	parser_context.event = event_create(ctx->mail->box->event);
	event_add_category(parser_context.event, &event_category_fts);
	T_BEGIN {
		const char *prefix =
			t_strdup_printf("fts-%s: ", ctx->update_ctx->backend->name);
		event_set_append_log_prefix(parser_context.event, prefix);
	} T_END;

	if (fts_parser_init(&parser_context, &ctx->body_parser)) {
		/* extract text using the returned parser */
		*binary_body_r = TRUE;
		key.type = FTS_BACKEND_BUILD_KEY_BODY_PART;
	} else if (str_begins_with(parser_context.content_type, "text/") ||
		   str_begins_with(parser_context.content_type, "message/")) {
		/* text body parts */
		key.type = FTS_BACKEND_BUILD_KEY_BODY_PART;
		ctx->body_parser = fts_parser_text_init();
	} else {
		/* possibly binary */
		if ((ctx->update_ctx->backend->flags &
		     FTS_BACKEND_FLAG_BINARY_MIME_PARTS) == 0) {
			event_unref(&parser_context.event);
			return FALSE;
		     }
		*binary_body_r = TRUE;
		key.type = FTS_BACKEND_BUILD_KEY_BODY_PART_BINARY;
	}
	key.body_content_type = parser_context.content_type;
	key.body_content_disposition = ctx->content_disposition;
	ctx->cur_user_lang = NULL;
	if (!fts_backend_update_set_build_key(ctx->update_ctx, &key)) {
		if (ctx->body_parser != NULL)
			(void)fts_parser_deinit(&ctx->body_parser, NULL);
		event_unref(&parser_context.event);
		return FALSE;
	}

	event_unref(&parser_context.event);
	return TRUE;
}

static int
fts_build_add_tokens_with_filter(struct fts_mail_build_context *ctx,
				 const unsigned char *data, size_t size)
{
	struct lang_tokenizer *tokenizer = ctx->cur_user_lang->index_tokenizer;
	struct lang_filter *filter = ctx->cur_user_lang->filter;
	const char *token, *error;
	int ret = 1, ret2;

	while (ret > 0) T_BEGIN {
		ret = ret2 = lang_tokenizer_next(tokenizer, data, size, &token, &error);
		if (ret2 > 0 && filter != NULL)
			ret2 = lang_filter(filter, &token, &error);
		if (ret2 < 0) {
			mail_set_critical(ctx->mail,
				"fts: Couldn't create indexable tokens: %s",
				error);
		}
		if (ret2 > 0) {
			if (fts_backend_update_build_more(ctx->update_ctx,
							  (const void *)token,
							  strlen(token)) < 0) {
				mail_storage_set_internal_error(ctx->mail->box->storage);
				ret = -1;
			}
		}
	} T_END;
	return ret;
}

static int
fts_detect_language(struct fts_mail_build_context *ctx,
		    const unsigned char *data, size_t size, bool last,
		    const struct language **lang_r)
{
	struct mail_user *user = ctx->update_ctx->backend->ns->user;
	struct language_list *lang_list = lang_user_get_language_list(user);
	const struct language *lang;
	const char *error;

	switch (language_detect(lang_list, data, size, &lang, &error)) {
	case LANGUAGE_DETECT_RESULT_SHORT:
		/* save the input so far and try again later */
		buffer_append(ctx->pending_input, data, size);
		if (last) {
			/* we've run out of data. use the default language. */
			*lang_r = language_list_get_first(lang_list);
			return 1;
		}
		return 0;
	case LANGUAGE_DETECT_RESULT_UNKNOWN:
		/* use the default language */
		*lang_r = language_list_get_first(lang_list);
		return 1;
	case LANGUAGE_DETECT_RESULT_OK:
		*lang_r = lang;
		return 1;
	case LANGUAGE_DETECT_RESULT_ERROR:
		/* internal language detection library failure
		   (e.g. invalid config). don't index anything. */
		mail_set_critical(ctx->mail,
			"Language detection library initialization failed: %s",
			error);
		return -1;
	default:
		i_unreached();
	}
}

static int
fts_build_tokenized(struct fts_mail_build_context *ctx,
		    const unsigned char *data, size_t size, bool last)
{
	struct mail_user *user = ctx->update_ctx->backend->ns->user;
	const struct language *lang;
	int ret;

	i_assert(ctx->pending_input != NULL);

	if (ctx->cur_user_lang != NULL) {
		/* we already have a language */
	} else if ((ret = fts_detect_language(ctx, data, size, last, &lang)) < 0) {
		return -1;
	} else if (ret == 0) {
		/* wait for more data */
		return 0;
	} else {
		fts_mail_build_ctx_set_lang(ctx, lang_user_language_find(user, lang));

		if (ctx->pending_input->used > 0) {
			if (fts_build_add_tokens_with_filter(ctx,
					ctx->pending_input->data,
					ctx->pending_input->used) < 0)
				return -1;
			buffer_set_used_size(ctx->pending_input, 0);
		}
	}
	if (fts_build_add_tokens_with_filter(ctx, data, size) < 0)
		return -1;
	if (last) {
		if (fts_build_add_tokens_with_filter(ctx, NULL, 0) < 0)
			return -1;
	}
	return 0;
}

static int
fts_build_full_words(struct fts_mail_build_context *ctx,
		     const unsigned char *data, size_t size, bool last)
{
	size_t i;

	/* we'll need to send only full words to the backend */

	if (ctx->word_buf != NULL && ctx->word_buf->used > 0) {
		/* continuing previous word */
		for (i = 0; i < size; i++) {
			if (IS_WORD_WHITESPACE(data[i]))
				break;
		}
		buffer_append(ctx->word_buf, data, i);
		data += i;
		size -= i;
		if (size == 0 && ctx->word_buf->used < MAX_WORD_SIZE && !last) {
			/* word is still not finished */
			return 0;
		}
		/* we have a full word, index it */
		if (fts_backend_update_build_more(ctx->update_ctx,
						  ctx->word_buf->data,
						  ctx->word_buf->used) < 0) {
			mail_storage_set_internal_error(ctx->mail->box->storage);
			return -1;
		}
		buffer_set_used_size(ctx->word_buf, 0);
	}

	/* find the boundary for last word */
	if (last)
		i = size;
	else {
		for (i = size; i > 0; i--) {
			if (IS_WORD_WHITESPACE(data[i-1]))
				break;
		}
	}

	if (fts_backend_update_build_more(ctx->update_ctx, data, i) < 0) {
		mail_storage_set_internal_error(ctx->mail->box->storage);
		return -1;
	}

	if (i < size) {
		if (ctx->word_buf == NULL) {
			ctx->word_buf =
				buffer_create_dynamic(default_pool, 128);
		}
		buffer_append(ctx->word_buf, data + i, size - i);
	}
	return 0;
}

static int fts_build_data(struct fts_mail_build_context *ctx,
			  const unsigned char *data, size_t size, bool last)
{
	if ((ctx->update_ctx->backend->flags &
	     FTS_BACKEND_FLAG_TOKENIZED_INPUT) != 0) {
		return fts_build_tokenized(ctx, data, size, last);
	} else if ((ctx->update_ctx->backend->flags &
		    FTS_BACKEND_FLAG_BUILD_FULL_WORDS) != 0) {
		return fts_build_full_words(ctx, data, size, last);
	} else {
		if (fts_backend_update_build_more(ctx->update_ctx, data, size) < 0) {
			mail_storage_set_internal_error(ctx->mail->box->storage);
			return -1;
		}
		return 0;
	}
}

static int fts_build_body_block(struct fts_mail_build_context *ctx,
				const struct message_block *block, bool last)
{
	i_assert(block->hdr == NULL);

	return fts_build_data(ctx, block->data, block->size, last);
}

static int fts_body_parser_finish(struct fts_mail_build_context *ctx,
				  const char **retriable_err_msg_r,
				  bool *may_need_retry_r)
{
	struct message_block block;
	const char *retriable_error;
	int ret = 0;
	int deinit_ret;
	*may_need_retry_r = FALSE;

	do {
		i_zero(&block);
		fts_parser_more(ctx->body_parser, &block);
		if (fts_build_body_block(ctx, &block, FALSE) < 0) {
			ret = -1;
			break;
		}
	} while (block.size > 0);

	deinit_ret = fts_parser_deinit(&ctx->body_parser, &retriable_error);
	if (ret < 0) {
		/* indexing already failed - we don't want to retry
		   in any case */
		return -1;
	}

	if (deinit_ret == 0) {
		/* retry the parsing */
		*may_need_retry_r = TRUE;
		*retriable_err_msg_r = retriable_error;
		return -1;
	}
	if (deinit_ret < 0) {
		mail_storage_set_internal_error(ctx->mail->box->storage);
		return -1;
	}
	return 0;
}

static void
parse_header_filter(const ARRAY_TYPE(const_string) *values, pool_t pool,
		    ARRAY_TYPE(const_string) *list_r, bool *matches_all_r)
{
	*matches_all_r = FALSE;
	if (array_is_empty(values))
		return;

	const char *entry;
	array_foreach_elem(values, entry) {
		const char *value = p_strdup(pool, t_str_lcase(entry));
		array_push_back(list_r, &value);
		if (*value == '*') {
			*matches_all_r = TRUE;
			break;
		}
	}
	array_sort(list_r, i_strcmp_p);
}

static struct fts_header_filters *
load_header_filters(struct fts_backend *backend)
{
	const struct fts_settings *set = fts_user_get_settings(backend->ns->user);
	struct fts_header_filters *filters = &backend->header_filters;
	if (!filters->loaded) {
		bool match_all;
		/* match_all used just as dummy output here */
		parse_header_filter(&set->header_includes, filters->pool,
				    &filters->includes, &match_all);

		/* match_all from this call is relevant instead */
		parse_header_filter(&set->header_excludes, filters->pool,
				    &filters->excludes, &match_all);

		filters->loaded = TRUE;
		filters->exclude_is_default = match_all;
	}
	return filters;
}

/* This performs comparison between two strings, where the second one can end
 * with the wildcard '*'. When the match reaches a '*' on the pitem side, zero
 * (match) is returned regardles of the remaining characters.
 *
 * The function obeys the same lexicographic order as i_strcmp_p() and
 * strcmp(), which is the reason for the casts to unsigned before comparing.
 */
static int ATTR_PURE
header_prefix_cmp(const char *const *pkey, const char *const *pitem)
{
	const char *key = *pkey;
	const char *item = *pitem;

	while (*key == *item && *key != '\0') key++, item++;
	return item[0] == '*' && item[1] == '\0' ? 0 :
	       (unsigned char)*key - (unsigned char)*item;
}

static bool
is_header_indexable(const char *header_name, struct fts_backend *backend)
{
	bool indexable;
	T_BEGIN {
		struct fts_header_filters *filters = load_header_filters(backend);
		const char *hdr = t_str_lcase(header_name);

		if (array_bsearch(&filters->includes, &hdr, header_prefix_cmp) != NULL)
			indexable = TRUE;
		else if (filters->exclude_is_default ||
		         array_bsearch(&filters->excludes, &hdr, header_prefix_cmp) != NULL)
			indexable = FALSE;
		else
			indexable = TRUE;
	} T_END;
	return indexable;
}

static int
fts_build_mail_real(struct fts_backend_update_context *update_ctx,
		    struct mail *mail,
		    const char **retriable_err_msg_r,
		    bool *may_need_retry_r)
{
	const struct message_parser_settings parser_set = {
		.hdr_flags = MESSAGE_HEADER_PARSER_FLAG_CLEAN_ONELINE,
	};
	struct fts_mail_build_context ctx;
	struct istream *input;
	struct message_parser_ctx *parser;
	struct message_decoder_context *decoder;
	struct message_block raw_block, block;
	struct message_part *prev_part, *parts;
	bool skip_body = FALSE, body_part = FALSE, body_added = FALSE;
	bool binary_body;
	const char *error;
	int ret;
	size_t fts_message_max_size, orig_fts_message_max_size;

	fts_message_max_size = orig_fts_message_max_size =
		fts_mail_user_message_max_size(update_ctx->cur_box->storage->user);

	*may_need_retry_r = FALSE;
	if (mail_get_stream_because(mail, NULL, NULL, "fts indexing", &input) < 0) {
		if (mail->expunged)
			return 0;
		mail_set_critical(mail, "Failed to read stream: %s",
			mail_get_last_internal_error(mail, NULL));
		return -1;
	}

	i_zero(&ctx);
	ctx.update_ctx = update_ctx;
	ctx.mail = mail;
	if ((update_ctx->backend->flags & FTS_BACKEND_FLAG_TOKENIZED_INPUT) != 0)
		ctx.pending_input = buffer_create_dynamic(default_pool, 128);

	prev_part = NULL;
	pool_t parts_pool = pool_alloconly_create("fts message parts", 512);
	parser = message_parser_init(parts_pool, input, &parser_set);

	decoder = message_decoder_init(update_ctx->normalizer, 0);
	for (;;) {
		ret = message_parser_parse_next_block(parser, &raw_block);
		i_assert(ret != 0);
		if (ret < 0) {
			if (input->stream_errno == 0)
				ret = 0;
			else {
				mail_set_critical(mail, "read(%s) failed: %s",
					i_stream_get_name(input),
					i_stream_get_error(input));
			}
			break;
		}

		if (raw_block.part != prev_part) {
			/* body part changed. we're now parsing the end of
			   boundary, possibly followed by message epilogue */
			if (ctx.body_parser != NULL) {
				if (fts_body_parser_finish(&ctx, retriable_err_msg_r,
							   may_need_retry_r) < 0) {
					ret = -1;
					break;
				}
			}
			message_decoder_set_return_binary(decoder, FALSE);
			fts_backend_update_unset_build_key(update_ctx);
			prev_part = raw_block.part;
			i_free_and_null(ctx.content_type);
			i_free_and_null(ctx.content_type_params);
			i_free_and_null(ctx.content_disposition);

			if (raw_block.size != 0) {
				/* multipart. skip until beginning of next
				   part's headers */
				skip_body = TRUE;
			}
		}

		if (raw_block.hdr != NULL) {
			/* always handle headers */
		} else if (raw_block.size == 0) {
			/* end of headers */
			skip_body = !fts_build_body_begin(&ctx, raw_block.part,
							  &binary_body);
			if (binary_body)
				message_decoder_set_return_binary(decoder, TRUE);
			body_part = TRUE;
		} else {
			if (skip_body)
				continue;
		}

		if (!message_decoder_decode_next_block(decoder, &raw_block,
						       &block))
			continue;

		/* If the block size exceeds limit, we truncate the block
		   which would have exceeded to final size, and parse it.
		   Then we ignore the rest body parts. */
		if (body_part && orig_fts_message_max_size > 0) {
			if (fts_message_max_size > block.size) {
				fts_message_max_size -= block.size;
			} else if (fts_message_max_size == 0) {
				continue;
			} else {
				e_debug(mail->box->event,
					"UID %u: Body size exceeds %zu, truncating and skipping",
					mail->uid, orig_fts_message_max_size);
				block.size = fts_message_max_size;
				fts_message_max_size = 0;
			}
		}

		if (block.hdr != NULL) {
			fts_parse_mail_header(&ctx, &raw_block);
			if (is_header_indexable(block.hdr->name, update_ctx->backend) &&
			    fts_build_mail_header(&ctx, &block) < 0) {
				ret = -1;
				break;
			}
		} else if (block.size == 0) {
			/* end of headers */
		} else {
			i_assert(body_part);
			if (ctx.body_parser != NULL)
				fts_parser_more(ctx.body_parser, &block);
			if (fts_build_body_block(&ctx, &block, FALSE) < 0) {
				ret = -1;
				break;
			}
			body_added = TRUE;
		}
	}
	if (ctx.body_parser != NULL) {
		if (ret == 0)
			ret = fts_body_parser_finish(&ctx, retriable_err_msg_r,
						     may_need_retry_r);
		else
			(void)fts_parser_deinit(&ctx.body_parser, NULL);
	}
	if (ret == 0 && body_part && !skip_body && !body_added) {
		/* make sure body is added even when it doesn't exist */
		block.data = NULL; block.size = 0;
		ret = fts_build_body_block(&ctx, &block, TRUE);
	}
	if (message_parser_deinit_from_parts(&parser, &parts, &error) < 0)
		index_mail_set_message_parts_corrupted(mail, error);
	message_decoder_deinit(&decoder);
	i_free(ctx.content_type);
	i_free(ctx.content_type_params);
	i_free(ctx.content_disposition);
	buffer_free(&ctx.word_buf);
	buffer_free(&ctx.pending_input);
	pool_unref(&parts_pool);
	return ret < 0 ? -1 : 1;
}

int fts_build_mail(struct fts_backend_update_context *update_ctx,
		   struct mail *mail)
{
	struct event *event = update_ctx->backend->event;
	int ret;
	/* Number of attempts to be taken if retry is needed */
	unsigned int attempts = 2;
	const char *retriable_err_msg;
	bool may_need_retry;

	T_BEGIN {
		while ((ret = fts_build_mail_real(update_ctx, mail,
						  &retriable_err_msg,
						  &may_need_retry)) < 0 &&
		       may_need_retry) {
			if (--attempts == 0) {
				/* Log this as info instead of as error,
				   because e.g. Tika doesn't differentiate
				   between temporary errors and invalid
				   document input. */
				e_info(event,
				       "Mailbox %s: UID %u: %s - ignoring",
				       mailbox_get_vname(mail->box), mail->uid,
				       retriable_err_msg);
				ret = 0;
				break;
			}
		}
	} T_END;
	return ret;
}
