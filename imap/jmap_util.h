/* jmap_util.h -- Helper routines for JMAP
 *
 * Copyright (c) 1994-2018 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef JMAP_UTIL_H
#define JMAP_UTIL_H

#include <jansson.h>

#include "hash.h"
#include "message.h"
#include "smtpclient.h"

#define JMAP_SUBMISSION_HDR "Content-Description"

#define jmap_wantprop(props, name) \
    ((props) ? (hash_lookup(name, props) != NULL) : 1)

#define jmap_readprop(root, name,  mandatory, invalid, fmt, dst) \
    jmap_readprop_full((root), NULL, (name), (mandatory), (invalid), (fmt), (dst))

extern int jmap_readprop_full(json_t *root, const char *prefix, const char *name,
                              int mandatory, json_t *invalid, const char *fmt,
                              void *dst);

/* Apply patch to a deep copy of val and return the result.
 * Return NULL on error. If invalid is a JSON array, then
 * the erroneous path in patch is appended as JSON string */
extern json_t* jmap_patchobject_apply(json_t *val, json_t *patch, json_t *invalid);

/* Create a patch-object that transforms src into dst. */
extern json_t *jmap_patchobject_create(json_t *src, json_t *dst);

/* Return non-zero src and its RFC6901 encoding differ */
extern int jmap_pointer_needsencode(const char *src);

/* Encode src according to RFC6901 */
extern char *jmap_pointer_encode(const char *src);

/* Decode src according to RFC6901 */
extern char *jmap_pointer_decode(const char *src, size_t len);

/* Remove all properties in jobj that have no key in props */
extern void jmap_filterprops(json_t *jobj, hash_table *props);

extern void jmap_emailsubmission_envelope_to_smtp(smtp_envelope_t *smtpenv,
                                                  json_t *env);

extern json_t *jmap_fetch_snoozed(const char *mbox, uint32_t uid);

extern int jmap_email_keyword_is_valid(const char *keyword);
extern const char *jmap_keyword_to_imap(const char *keyword);

/* JMAP request parser */
struct jmap_parser {
    struct buf buf;
    strarray_t path;
    json_t *invalid;
};

#define JMAP_PARSER_INITIALIZER { BUF_INITIALIZER, STRARRAY_INITIALIZER, json_array() }

extern void jmap_parser_fini(struct jmap_parser *parser);
extern void jmap_parser_push(struct jmap_parser *parser, const char *prop);
extern void jmap_parser_push_index(struct jmap_parser *parser,
                                   const char *prop, size_t index, const char *name);
extern void jmap_parser_pop(struct jmap_parser *parser);
extern const char* jmap_parser_path(struct jmap_parser *parser, struct buf *buf);
extern void jmap_parser_invalid(struct jmap_parser *parser, const char *prop);

extern json_t *jmap_server_error(int r);

extern char *jmap_encode_base64_nopad(const char *data, size_t len);
extern char *jmap_decode_base64_nopad(const char *b64, size_t b64len);

#endif /* JMAP_UTIL_H */
