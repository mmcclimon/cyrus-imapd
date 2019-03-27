/* http_jmap.c -- Routines for handling JMAP requests in httpd
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

#include <config.h>

#include <errno.h>

#include "acl.h"
#include "append.h"
#include "httpd.h"
#include "http_jmap.h"
#include "http_proxy.h"
#include "http_ws.h"
#include "mboxname.h"
#include "proxy.h"
#include "times.h"
#include "syslog.h"
#include "xstrlcpy.h"

/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"
#include "imap/imap_err.h"
#include "imap/jmap_err.h"


#define JMAP_ROOT          "/jmap"
#define JMAP_BASE_URL      JMAP_ROOT "/"
#define JMAP_WS_COL        "ws/"
#define JMAP_UPLOAD_COL    "upload/"
#define JMAP_UPLOAD_TPL    "{accountId}/"
#define JMAP_DOWNLOAD_COL  "download/"
#define JMAP_DOWNLOAD_TPL  "{accountId}/{blobId}/{name}?accept={type}"

struct namespace jmap_namespace;

static time_t compile_time;


/* Namespace callbacks */
static void jmap_init(struct buf *serverinfo);
static int  jmap_need_auth(struct transaction_t *txn);
static int  jmap_auth(const char *userid);
static void jmap_shutdown(void);

/* HTTP method handlers */
static int meth_get(struct transaction_t *txn, void *params);
static int meth_options_jmap(struct transaction_t *txn, void *params);
static int meth_post(struct transaction_t *txn, void *params);

/* JMAP Requests */
static int jmap_settings(struct transaction_t *txn);
static int jmap_download(struct transaction_t *txn);
static int jmap_upload(struct transaction_t *txn);

/* JMAP Core API Methods */
static int jmap_blob_get(jmap_req_t *req);
static int jmap_blob_copy(jmap_req_t *req);
static int jmap_core_echo(jmap_req_t *req);

/* WebSocket handler */
#define JMAP_WS_PROTOCOL   "jmap"

static int jmap_ws(struct buf *inbuf, struct buf *outbuf,
                   struct buf *logbuf, void **rock);

static struct connect_params ws_params = {
    JMAP_BASE_URL JMAP_WS_COL, JMAP_WS_PROTOCOL, &jmap_ws
};


/* Namespace for JMAP */
struct namespace_t namespace_jmap = {
    URL_NS_JMAP, 0, "jmap", JMAP_ROOT, "/.well-known/jmap",
    jmap_need_auth, /*authschemes*/0,
    /*mbtype*/0, 
    (ALLOW_READ | ALLOW_POST),
    &jmap_init, &jmap_auth, NULL, &jmap_shutdown, NULL, /*bearer*/NULL,
    {
        { NULL,                 NULL },                 /* ACL          */
        { NULL,                 NULL },                 /* BIND         */
        { &meth_connect,        &ws_params },           /* CONNECT      */
        { NULL,                 NULL },                 /* COPY         */
        { NULL,                 NULL },                 /* DELETE       */
        { &meth_get,            NULL },                 /* GET          */
        { &meth_get,            NULL },                 /* HEAD         */
        { NULL,                 NULL },                 /* LOCK         */
        { NULL,                 NULL },                 /* MKCALENDAR   */
        { NULL,                 NULL },                 /* MKCOL        */
        { NULL,                 NULL },                 /* MOVE         */
        { &meth_options_jmap,   NULL },                 /* OPTIONS      */
        { NULL,                 NULL },                 /* PATCH        */
        { &meth_post,           NULL },                 /* POST         */
        { NULL,                 NULL },                 /* PROPFIND     */
        { NULL,                 NULL },                 /* PROPPATCH    */
        { NULL,                 NULL },                 /* PUT          */
        { NULL,                 NULL },                 /* REPORT       */
        { &meth_trace,          NULL },                 /* TRACE        */
        { NULL,                 NULL },                 /* UNBIND       */
        { NULL,                 NULL }                  /* UNLOCK       */
    }
};


/*
 * Namespace callbacks
 */

static jmap_settings_t my_jmap_settings = {
    HASH_TABLE_INITIALIZER, STRARRAY_INITIALIZER, NULL, { 0 }
};

jmap_method_t jmap_core_methods[] = {
    { "Blob/copy",    &jmap_blob_copy, 0/*flags*/ },
    { "Blob/get",     &jmap_blob_get,  JMAP_SHARED_CSTATE },
    { "Core/echo",    &jmap_core_echo, JMAP_SHARED_CSTATE },
    { NULL,           NULL, 0/*flags*/ }
};

static void jmap_core_init()
{
#define _read_opt(val, optkey) \
    val = config_getint(optkey); \
    if (val <= 0) { \
        syslog(LOG_ERR, "jmap: invalid property value: %s", \
                imapopts[optkey].optname); \
        val = 0; \
    }
    _read_opt(my_jmap_settings.limits[MAX_SIZE_UPLOAD],
              IMAPOPT_JMAP_MAX_SIZE_UPLOAD);
    my_jmap_settings.limits[MAX_SIZE_UPLOAD] *= 1024;
    _read_opt(my_jmap_settings.limits[MAX_CONCURRENT_UPLOAD],
              IMAPOPT_JMAP_MAX_CONCURRENT_UPLOAD);
    _read_opt(my_jmap_settings.limits[MAX_SIZE_REQUEST],
              IMAPOPT_JMAP_MAX_SIZE_REQUEST);
    my_jmap_settings.limits[MAX_SIZE_REQUEST] *= 1024;
    _read_opt(my_jmap_settings.limits[MAX_CONCURRENT_REQUESTS],
              IMAPOPT_JMAP_MAX_CONCURRENT_REQUESTS);
    _read_opt(my_jmap_settings.limits[MAX_CALLS_IN_REQUEST],
              IMAPOPT_JMAP_MAX_CALLS_IN_REQUEST);
    _read_opt(my_jmap_settings.limits[MAX_OBJECTS_IN_GET],
              IMAPOPT_JMAP_MAX_OBJECTS_IN_GET);
    _read_opt(my_jmap_settings.limits[MAX_OBJECTS_IN_SET],
              IMAPOPT_JMAP_MAX_OBJECTS_IN_SET);
#undef _read_opt

    strarray_push(&my_jmap_settings.can_use, JMAP_URN_CORE);
 
    construct_hash_table(&my_jmap_settings.methods, 128, 0);

    jmap_method_t *mp;
    for (mp = jmap_core_methods; mp->name; mp++) {
        hash_insert(mp->name, mp, &my_jmap_settings.methods);
    }
}

static void jmap_core_capabilities()
{
    my_jmap_settings.capabilities =
        json_pack("{s:{s:i s:i s:i s:i s:i s:i s:i s:o}}",
                  JMAP_URN_CORE,
                  "maxSizeUpload",
                  my_jmap_settings.limits[MAX_SIZE_UPLOAD],
                  "maxConcurrentUpload",
                  my_jmap_settings.limits[MAX_CONCURRENT_UPLOAD],
                  "maxSizeRequest",
                  my_jmap_settings.limits[MAX_SIZE_REQUEST],
                  "maxConcurrentRequests",
                  my_jmap_settings.limits[MAX_CONCURRENT_REQUESTS],
                  "maxCallsInRequest",
                  my_jmap_settings.limits[MAX_CALLS_IN_REQUEST],
                  "maxObjectsInGet",
                  my_jmap_settings.limits[MAX_OBJECTS_IN_GET],
                  "maxObjectsInSet",
                  my_jmap_settings.limits[MAX_OBJECTS_IN_SET],
                  "collationAlgorithms", json_array()
            );

    if (ws_enabled()) {
        json_object_set_new(my_jmap_settings.capabilities, JMAP_URN_WEBSOCKET,
                            json_pack("{s:s}", "wsUrl", JMAP_BASE_URL JMAP_WS_COL));
    }

    json_object_set_new(my_jmap_settings.capabilities,
                        XML_NS_CYRUS "performance", json_object());
}

static void jmap_init(struct buf *serverinfo __attribute__((unused)))
{
    namespace_jmap.enabled =
        config_httpmodules & IMAP_ENUM_HTTPMODULES_JMAP;

    if (!namespace_jmap.enabled) return;

    compile_time = calc_compile_time(__TIME__, __DATE__);

    initialize_JMAP_error_table();

    jmap_core_init();
    jmap_user_init(&my_jmap_settings);
    jmap_mail_init(&my_jmap_settings);
    jmap_contact_init(&my_jmap_settings);
    jmap_calendar_init(&my_jmap_settings);
}

static int jmap_auth(const char *userid __attribute__((unused)))
{
    /* Set namespace */
    mboxname_init_namespace(&jmap_namespace,
                            httpd_userisadmin || httpd_userisproxyadmin);
    return 0;
}

static int jmap_need_auth(struct transaction_t *txn __attribute__((unused)))
{
    /* All endpoints require authentication */
    return HTTP_UNAUTHORIZED;
}

static void jmap_shutdown(void)
{
    free_hash_table(&my_jmap_settings.methods, NULL);
    strarray_fini(&my_jmap_settings.can_use);
    if (my_jmap_settings.capabilities)
        json_decref(my_jmap_settings.capabilities);
}   


/*
 * HTTP method handlers
 */

enum {
    JMAP_ENDPOINT_API,
    JMAP_ENDPOINT_WS,
    JMAP_ENDPOINT_UPLOAD,
    JMAP_ENDPOINT_DOWNLOAD
};

static int jmap_parse_path(struct transaction_t *txn)
{
    struct request_target_t *tgt = &txn->req_tgt;
    size_t len;
    char *p;

    if (*tgt->path) return 0;  /* Already parsed */

    /* Make a working copy of target path */
    strlcpy(tgt->path, txn->req_uri->path, sizeof(tgt->path));
    p = tgt->path;

    /* Sanity check namespace */
    len = strlen(namespace_jmap.prefix);
    if (strlen(p) < len ||
        strncmp(namespace_jmap.prefix, p, len) ||
        (tgt->path[len] && tgt->path[len] != '/')) {
        txn->error.desc = "Namespace mismatch request target path";
        return HTTP_FORBIDDEN;
    }

    /* Skip namespace */
    p += len;
    if (!*p) {
        /* Canonicalize URL */
        txn->location = JMAP_BASE_URL;
        return HTTP_MOVED;
    }

    /* Check for path after prefix */
    if (*++p) {
        /* Get "collection" */
        tgt->collection = p;

        if (!strncmp(tgt->collection, JMAP_UPLOAD_COL,
                          strlen(JMAP_UPLOAD_COL))) {
            tgt->flags = JMAP_ENDPOINT_UPLOAD;
            tgt->allow = ALLOW_POST;

            /* Get "resource" which must be the accountId */
            tgt->resource = tgt->collection + strlen(JMAP_UPLOAD_COL);
        }
        else if (!strncmp(tgt->collection,
                          JMAP_DOWNLOAD_COL, strlen(JMAP_DOWNLOAD_COL))) {
            tgt->flags = JMAP_ENDPOINT_DOWNLOAD;
            tgt->allow = ALLOW_READ;

            /* Get "resource" */
            tgt->resource = tgt->collection + strlen(JMAP_DOWNLOAD_COL);
        }
        else if (ws_enabled() && !strcmp(tgt->collection, JMAP_WS_COL)) {
            tgt->flags = JMAP_ENDPOINT_WS;
            tgt->allow = (txn->flags.ver == VER_2) ? ALLOW_CONNECT : ALLOW_READ;
        }
        else {
            return HTTP_NOT_FOUND;
        }
    }
    else {
        tgt->flags = JMAP_ENDPOINT_API;
        tgt->allow = ALLOW_POST|ALLOW_READ;
    }

    return 0;
}

/* Perform a GET/HEAD request */
static int meth_get(struct transaction_t *txn,
                    void *params __attribute__((unused)))
{
    int r = jmap_parse_path(txn);

    if (!(txn->req_tgt.allow & ALLOW_READ)) {
        return HTTP_NOT_FOUND;
    }
    else if (r) return r;

    if (txn->req_tgt.flags == JMAP_ENDPOINT_API) {
        return jmap_settings(txn);
    }
    else if (txn->req_tgt.flags == JMAP_ENDPOINT_DOWNLOAD) {
        return jmap_download(txn);
    }
    /* Upgrade to WebSockets over HTTP/1.1 on WS endpoint, if requested */
    else if ((txn->req_tgt.flags == JMAP_ENDPOINT_WS) &&
             (txn->flags.upgrade & UPGRADE_WS)) {
        return ws_start_channel(txn, JMAP_WS_PROTOCOL, &jmap_ws);
    }

    return HTTP_NO_CONTENT;
}

static int json_response(int code, struct transaction_t *txn, json_t *root)
{
    size_t flags = JSON_PRESERVE_ORDER;
    char *buf;

    /* Dump JSON object into a text buffer */
    flags |= (config_httpprettytelemetry ? JSON_INDENT(2) : JSON_COMPACT);
    buf = json_dumps(root, flags);
    json_decref(root);

    if (!buf) {
        txn->error.desc = "Error dumping JSON object";
        return HTTP_SERVER_ERROR;
    }

    /* Output the JSON object */
    switch (code) {
    case HTTP_OK:
    case HTTP_CREATED:
        txn->resp_body.type = "application/json; charset=utf-8";
        break;
    default:
        txn->resp_body.type = "application/problem+json; charset=utf-8";
        break;
    }

    write_body(code, txn, buf, strlen(buf));
    free(buf);

    return 0;
}

/* Perform a POST request */
static int meth_post(struct transaction_t *txn,
                     void *params __attribute__((unused)))
{
    int ret;
    json_t *res = NULL;

    ret = jmap_parse_path(txn);

    if (ret) return ret;
    if (!(txn->req_tgt.allow & ALLOW_POST)) {
        return HTTP_NOT_ALLOWED;
    }

    /* Handle uploads */
    if (txn->req_tgt.flags == JMAP_ENDPOINT_UPLOAD) {
        return jmap_upload(txn);
    }

    /* Regular JMAP API request */
    ret = jmap_api(txn, &res, &my_jmap_settings);

    if (!ret) {
        /* Output the JSON object */
        ret = json_response(HTTP_OK, txn, res);
    }

    syslog(LOG_DEBUG, ">>>> jmap_post: Exit\n");
    return ret;
}

/* Perform an OPTIONS request */
static int meth_options_jmap(struct transaction_t *txn, void *params)
{
    /* Parse the path */
    int r = jmap_parse_path(txn);
    if (r) return r;

    return meth_options(txn, params);
}


/*
 * JMAP Requests
 */

static char *parse_accept_header(const char **hdr)
{
    char *val = NULL;
    struct accept *accept = parse_accept(hdr);
    if (accept) {
        char *type = NULL;
        char *subtype = NULL;
        struct param *params = NULL;
        message_parse_type(accept->token, &type, &subtype, &params);
        if (type && subtype && !strchr(type, '*') && !strchr(subtype, '*'))
            val = xstrdup(accept->token);
        free(type);
        free(subtype);
        param_free(&params);
        struct accept *tmp;
        for (tmp = accept; tmp && tmp->token; tmp++) {
            free(tmp->token);
        }
        free(accept);
    }
    return val;
}

/* Handle a GET on the download endpoint */
static int jmap_download(struct transaction_t *txn)
{
    const char *userid = txn->req_tgt.resource;
    const char *slash = strchr(userid, '/');
    if (!slash) {
        /* XXX - error, needs AccountId */
        return HTTP_NOT_FOUND;
    }

#if 0
    size_t userlen = slash - userid;

    /* invalid user? */
    if (!strncmp(userid, httpd_userid, userlen)) {
        txn->error.desc = "failed to match userid";
        return HTTP_BAD_REQUEST;
    }
#endif

    const char *blobbase = slash + 1;
    slash = strchr(blobbase, '/');
    if (!slash) {
        /* XXX - error, needs blobid */
        txn->error.desc = "failed to find blobid";
        return HTTP_BAD_REQUEST;
    }
    size_t bloblen = slash - blobbase;

    if (*blobbase != 'G') {
        txn->error.desc = "invalid blobid (doesn't start with G)";
        return HTTP_BAD_REQUEST;
    }

    if (bloblen != 41) {
        /* incomplete or incorrect blobid */
        txn->error.desc = "invalid blobid (not 41 chars)";
        return HTTP_BAD_REQUEST;
    }

    const char *name = slash + 1;

    char *accountid = xstrndup(userid, strchr(userid, '/') - userid);
    int res = 0;

    struct conversations_state *cstate = NULL;
    int r = conversations_open_user(accountid, 1/*shared*/, &cstate);
    if (r) {
        txn->error.desc = error_message(r);
        res = (r == IMAP_MAILBOX_BADNAME) ? HTTP_NOT_FOUND : HTTP_SERVER_ERROR;
        free(accountid);
        return res;
    }

    /* now we're allocating memory, so don't return from here! */

    char *blobid = NULL;
    char *ctype = NULL;

    /* Initialize request context */
    struct jmap_req req;
    jmap_initreq(&req);

    req.userid = httpd_userid;
    req.accountid = accountid;
    req.cstate = cstate;
    req.authstate = httpd_authstate;
    req.txn = txn;


    /* Initialize ACL mailbox cache for findblob */
    hash_table mboxrights = HASH_TABLE_INITIALIZER;
    construct_hash_table(&mboxrights, 64, 0);
    req.mboxrights = &mboxrights;

    blobid = xstrndup(blobbase, bloblen);

    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    struct body *body = NULL;
    const struct body *part = NULL;
    struct buf msg_buf = BUF_INITIALIZER;
    char *decbuf = NULL;
    strarray_t headers = STRARRAY_INITIALIZER;
    char *accept_mime = NULL;

    /* Find part containing blob */
    r = jmap_findblob(&req, NULL/*accountid*/, blobid,
                      &mbox, &mr, &body, &part, &msg_buf);
    if (r) {
        res = HTTP_NOT_FOUND; // XXX errors?
        txn->error.desc = "failed to find blob by id";
        goto done;
    }

    if (!buf_base(&msg_buf)) {
        /* Map the message into memory */
        r = msgrecord_get_body(mr, &msg_buf);
        if (r) {
            res = HTTP_NOT_FOUND; // XXX errors?
            txn->error.desc = "failed to map record";
            goto done;
        }
    }

    struct strlist *param;
    if ((param = hash_lookup("accept", &txn->req_qparams))) {
        accept_mime = xstrdup(param->s);
    }

    const char **hdr;
    if (!accept_mime && (hdr = spool_getheader(txn->req_hdrs, "Accept"))) {
        accept_mime = parse_accept_header(hdr);
    }
    if (!accept_mime) accept_mime = xstrdup("application/octet-stream");

    // default with no part is the whole message
    const char *base = msg_buf.s;
    size_t len = msg_buf.len;
    txn->resp_body.type = accept_mime;

    if (part) {
        // map into just this part
        base += part->content_offset;
        len = part->content_size;

        // binary decode if needed
        int encoding = part->charset_enc & 0xff;
        base = charset_decode_mimebody(base, len, encoding, &decbuf, &len);
    }

    txn->resp_body.len = len;
    txn->resp_body.dispo.fname = name;

    write_body(HTTP_OK, txn, base, len);

 done:
    free(accept_mime);
    free_hash_table(&mboxrights, free);
    free(accountid);
    free(decbuf);
    free(ctype);
    strarray_fini(&headers);
    if (mbox) jmap_closembox(&req, &mbox);
    conversations_commit(&cstate);
    if (body) {
        message_free_body(body);
        free(body);
    }
    if (mr) {
        msgrecord_unref(&mr);
    }
    buf_free(&msg_buf);
    free(blobid);
    jmap_finireq(&req);
    return res;
}

static int lookup_upload_collection(const char *accountid, mbentry_t **mbentry)
{
    mbname_t *mbname;
    const char *uploadname;
    int r;

    /* Create notification mailbox name from the parsed path */
    mbname = mbname_from_userid(accountid);
    mbname_push_boxes(mbname, config_getstring(IMAPOPT_JMAPUPLOADFOLDER));

    /* XXX - hack to allow @domain parts for non-domain-split users */
    if (httpd_extradomain) {
        /* not allowed to be cross domain */
        if (mbname_localpart(mbname) &&
            strcmpsafe(mbname_domain(mbname), httpd_extradomain)) {
            r = HTTP_NOT_FOUND;
            goto done;
        }
        mbname_set_domain(mbname, NULL);
    }

    /* Locate the mailbox */
    uploadname = mbname_intname(mbname);
    r = http_mlookup(uploadname, mbentry, NULL);
    if (r == IMAP_MAILBOX_NONEXISTENT) {
        /* Find location of INBOX */
        char *inboxname = mboxname_user_mbox(accountid, NULL);

        int r1 = http_mlookup(inboxname, mbentry, NULL);
        free(inboxname);
        if (r1 == IMAP_MAILBOX_NONEXISTENT) {
            r = IMAP_INVALID_USER;
            goto done;
        }

        int rights = httpd_myrights(httpd_authstate, *mbentry);
        if (!(rights & ACL_CREATE)) {
            r = IMAP_PERMISSION_DENIED;
            goto done;
        }

        if (*mbentry) free((*mbentry)->name);
        else *mbentry = mboxlist_entry_create();
        (*mbentry)->name = xstrdup(uploadname);
    }
    else if (!r) {
        int rights = httpd_myrights(httpd_authstate, *mbentry);
        if (!(rights & ACL_INSERT)) {
            r = IMAP_PERMISSION_DENIED;
            goto done;
        }
    }

  done:
    mbname_free(&mbname);
    return r;
}


static int create_upload_collection(const char *accountid,
                                    struct mailbox **mailbox)
{
    /* notifications collection */
    mbentry_t *mbentry = NULL;
    int r = lookup_upload_collection(accountid, &mbentry);

    if (r == IMAP_INVALID_USER) {
        goto done;
    }
    else if (r == IMAP_PERMISSION_DENIED) {
        goto done;
    }
    else if (r == IMAP_MAILBOX_NONEXISTENT) {
        if (!mbentry) goto done;
        else if (mbentry->server) {
            proxy_findserver(mbentry->server, &http_protocol, httpd_userid,
                             &backend_cached, NULL, NULL, httpd_in);
            goto done;
        }

        r = mboxlist_createmailbox(mbentry->name, MBTYPE_COLLECTION,
                                   NULL, 1 /* admin */, accountid,
                                   httpd_authstate, 0, 0, 0, 0, mailbox);
        /* we lost the race, that's OK */
        if (r == IMAP_MAILBOX_LOCKED) r = 0;
        else {
            if (r) {
                syslog(LOG_ERR, "IOERROR: failed to create %s (%s)",
                        mbentry->name, error_message(r));
            }
            goto done;
        }
    }
    else if (r) goto done;

    if (mailbox) {
        /* Open mailbox for writing */
        r = mailbox_open_iwl(mbentry->name, mailbox);
        if (r) {
            syslog(LOG_ERR, "mailbox_open_iwl(%s) failed: %s",
                   mbentry->name, error_message(r));
        }
    }

 done:
    mboxlist_entry_free(&mbentry);
    return r;
}

/* Helper function to determine domain of data */
enum {
    DOMAIN_7BIT = 0,
    DOMAIN_8BIT,
    DOMAIN_BINARY
};

static int data_domain(const char *p, size_t n)
{
    int r = DOMAIN_7BIT;

    while (n--) {
        if (!*p) return DOMAIN_BINARY;
        if (*p & 0x80) r = DOMAIN_8BIT;
        p++;
    }

    return r;
}

/* Handle a POST on the upload endpoint */
static int jmap_upload(struct transaction_t *txn)
{
    strarray_t flags = STRARRAY_INITIALIZER;

    struct body *body = NULL;

    int ret = HTTP_CREATED;
    hdrcache_t hdrcache = txn->req_hdrs;
    struct stagemsg *stage = NULL;
    FILE *f = NULL;
    const char **hdr;
    time_t now = time(NULL);
    struct appendstate as;

    struct mailbox *mailbox = NULL;
    int r = 0;

    /* Read body */
    txn->req_body.flags |= BODY_DECODE;
    r = http_read_req_body(txn);
    if (r) {
        txn->flags.conn = CONN_CLOSE;
        return r;
    }

    const char *data = buf_base(&txn->req_body.payload);
    size_t datalen = buf_len(&txn->req_body.payload);

    if (datalen > (size_t) my_jmap_settings.limits[MAX_SIZE_UPLOAD]) {
        txn->error.desc = "JSON upload byte size exceeds maxSizeUpload";
        return HTTP_PAYLOAD_TOO_LARGE;
    }

    /* Resource must be {accountId}/ with no trailing path */
    char *accountid = xstrdup(txn->req_tgt.resource);
    char *slash = strchr(accountid, '/');
    if (!slash || *(slash + 1) != '\0') {
        ret = HTTP_NOT_FOUND;
        goto done;
    }
    *slash = '\0';

    r = create_upload_collection(accountid, &mailbox);
    if (r) {
        syslog(LOG_ERR, "jmap_upload: can't open upload collection for %s: %s",
               error_message(r), accountid);
        ret = HTTP_NOT_FOUND;
        goto done;
    }

    /* Prepare to stage the message */
    if (!(f = append_newstage(mailbox->name, now, 0, &stage))) {
        syslog(LOG_ERR, "append_newstage(%s) failed", mailbox->name);
        txn->error.desc = "append_newstage() failed";
        ret = HTTP_SERVER_ERROR;
        goto done;
    }

    /* Create RFC 5322 header for resource */
    if ((hdr = spool_getheader(hdrcache, "User-Agent"))) {
        fprintf(f, "User-Agent: %s\r\n", hdr[0]);
    }

    if ((hdr = spool_getheader(hdrcache, "From"))) {
        fprintf(f, "From: %s\r\n", hdr[0]);
    }
    else {
        char *mimehdr;

        assert(!buf_len(&txn->buf));
        if (strchr(httpd_userid, '@')) {
            /* XXX  This needs to be done via an LDAP/DB lookup */
            buf_printf(&txn->buf, "<%s>", httpd_userid);
        }
        else {
            buf_printf(&txn->buf, "<%s@%s>", httpd_userid, config_servername);
        }

        mimehdr = charset_encode_mimeheader(buf_cstring(&txn->buf),
                                            buf_len(&txn->buf), 0);
        fprintf(f, "From: %s\r\n", mimehdr);
        free(mimehdr);
        buf_reset(&txn->buf);
    }

    if ((hdr = spool_getheader(hdrcache, "Subject"))) {
        fprintf(f, "Subject: %s\r\n", hdr[0]);
    }

    if ((hdr = spool_getheader(hdrcache, "Date"))) {
        fprintf(f, "Date: %s\r\n", hdr[0]);
    }
    else {
        char datestr[80];
        time_to_rfc5322(now, datestr, sizeof(datestr));
        fprintf(f, "Date: %s\r\n", datestr);
    }

    if ((hdr = spool_getheader(hdrcache, "Message-ID"))) {
        fprintf(f, "Message-ID: %s\r\n", hdr[0]);
    }

    const char *type = "application/octet-stream";
    if ((hdr = spool_getheader(hdrcache, "Content-Type"))) {
        type = hdr[0];
    }
    fprintf(f, "Content-Type: %s\r\n", type);

    int domain = data_domain(data, datalen);
    switch (domain) {
        case DOMAIN_BINARY:
            fputs("Content-Transfer-Encoding: BINARY\r\n", f);
            break;
        case DOMAIN_8BIT:
            fputs("Content-Transfer-Encoding: 8BIT\r\n", f);
            break;
        default:
            break; // no CTE == 7bit
    }

    if ((hdr = spool_getheader(hdrcache, "Content-Disposition"))) {
        fprintf(f, "Content-Disposition: %s\r\n", hdr[0]);
    }

    if ((hdr = spool_getheader(hdrcache, "Content-Description"))) {
        fprintf(f, "Content-Description: %s\r\n", hdr[0]);
    }

    fprintf(f, "Content-Length: %u\r\n", (unsigned) datalen);

    fputs("MIME-Version: 1.0\r\n\r\n", f);

    /* Write the data to the file */
    fwrite(data, datalen, 1, f);
    fclose(f);

    /* Prepare to append the message to the mailbox */
    r = append_setup_mbox(&as, mailbox, httpd_userid, httpd_authstate,
                          0, /*quota*/NULL, 0, 0, /*event*/0);
    if (r) {
        syslog(LOG_ERR, "append_setup(%s) failed: %s",
               mailbox->name, error_message(r));
        ret = HTTP_SERVER_ERROR;
        txn->error.desc = "append_setup() failed";
        goto done;
    }

    /* Append the message to the mailbox */
    strarray_append(&flags, "\\Deleted");
    strarray_append(&flags, "\\Expunged");  // custom flag to insta-expunge!
    r = append_fromstage(&as, &body, stage, now, 0, &flags, 0, /*annots*/NULL);

    if (r) {
        append_abort(&as);
        syslog(LOG_ERR, "append_fromstage(%s) failed: %s",
               mailbox->name, error_message(r));
        ret = HTTP_SERVER_ERROR;
        txn->error.desc = "append_fromstage() failed";
        goto done;
    }

    r = append_commit(&as);
    if (r) {
        syslog(LOG_ERR, "append_commit(%s) failed: %s",
               mailbox->name, error_message(r));
        ret = HTTP_SERVER_ERROR;
        txn->error.desc = "append_commit() failed";
        goto done;
    }

    char datestr[RFC3339_DATETIME_MAX];
    time_to_rfc3339(now + 86400, datestr, RFC3339_DATETIME_MAX);

    char blob_id[JMAP_BLOBID_SIZE];
    jmap_set_blobid(&body->content_guid, blob_id);

    /* Create response object */
    json_t *resp = json_pack("{s:s}", "accountId", accountid);
    json_object_set_new(resp, "blobId", json_string(blob_id));
    json_object_set_new(resp, "size", json_integer(datalen));
    json_object_set_new(resp, "expires", json_string(datestr));

    /* Remove CFWS and encodings from type */
    char *normalisedtype = charset_decode_mimeheader(type, CHARSET_SNIPPET);
    json_object_set_new(resp, "type", json_string(normalisedtype));
    free(normalisedtype);

    /* Output the JSON object */
    ret = json_response(HTTP_CREATED, txn, resp);

done:
    free(accountid);
    if (body) {
        message_free_body(body);
        free(body);
    }
    strarray_fini(&flags);
    append_removestage(stage);
    if (mailbox) {
        if (r) mailbox_abort(mailbox);
        else r = mailbox_commit(mailbox);
        mailbox_close(&mailbox);
    }

    return ret;
}

struct findaccounts_data {
    json_t *accounts;
    struct buf userid;
    int rw;
    int has_mail;
    int has_contacts;
    int has_calendars;
};

static void findaccounts_add(struct findaccounts_data *ctx)
{
    if (!buf_len(&ctx->userid))
        return;

    const char *userid = buf_cstring(&ctx->userid);

    json_t *has_data_for = json_array();
    if (ctx->has_mail) {
        json_array_append_new(has_data_for, json_string(JMAP_URN_MAIL));
        json_array_append_new(has_data_for, json_string(JMAP_URN_SUBMISSION));
    }
    if (ctx->has_contacts)
        json_array_append_new(has_data_for, json_string(JMAP_URN_CONTACTS));
    if (ctx->has_calendars)
        json_array_append_new(has_data_for, json_string(JMAP_URN_CALENDARS));

    json_t *account = json_object();
    json_object_set_new(account, "name", json_string(userid));
    json_object_set_new(account, "isPrimary", json_false());
    json_object_set_new(account, "isReadOnly", json_boolean(!ctx->rw));
    json_object_set_new(account, "hasDataFor", has_data_for);

    json_object_set_new(ctx->accounts, userid, account);
}

static int findaccounts_cb(struct findall_data *data, void *rock)
{
    if (!data || !data->mbentry)
        return 0;

    const mbentry_t *mbentry = data->mbentry;
    mbname_t *mbname = mbname_from_intname(mbentry->name);
    const char *userid = mbname_userid(mbname);
    struct findaccounts_data *ctx = rock;
    const strarray_t *boxes = mbname_boxes(mbname);

    if (strcmp(buf_cstring(&ctx->userid), userid)) {
        /* We haven't yet seen this account.
           Add any previous account and reset state */
        findaccounts_add(ctx);
        buf_setcstr(&ctx->userid, userid);
        ctx->rw = 0;
        ctx->has_mail = 0;
        ctx->has_contacts = 0;
        ctx->has_calendars = 0;
    }

    if (!ctx->rw) {
        ctx->rw = httpd_myrights(httpd_authstate, data->mbentry) & ACL_READ_WRITE;
    }
    if (!ctx->has_mail) {
        ctx->has_mail = mbentry->mbtype == MBTYPE_EMAIL;
    }
    if (!ctx->has_contacts) {
        /* Only count children of user.foo.#addressbooks */
        const char *prefix = config_getstring(IMAPOPT_ADDRESSBOOKPREFIX);
        ctx->has_contacts =
            strarray_size(boxes) > 1 && !strcmpsafe(prefix, strarray_nth(boxes, 0));
    }
    if (!ctx->has_calendars) {
        /* Only count children of user.foo.#calendars */
        const char *prefix = config_getstring(IMAPOPT_CALENDARPREFIX);
        ctx->has_calendars =
            strarray_size(boxes) > 1 && !strcmpsafe(prefix, strarray_nth(boxes, 0));
    }

    mbname_free(&mbname);
    return 0;
}

static json_t *user_settings(const char *userid)
{
    json_t *accounts = json_pack("{s:{s:s s:b s:b s:[s,s,s,s]}}",
            userid, "name", userid,
            "isPrimary", 1,
            "isReadOnly", 0,
            /* JMAP autoprovisions calendars and contacts,
             * so these JMAP types always are available
             * for the primary account */
            "hasDataFor",
            JMAP_URN_MAIL,
            JMAP_URN_SUBMISSION,
            JMAP_URN_CONTACTS,
            JMAP_URN_CALENDARS);

    /* Find all shared accounts */
    strarray_t patterns = STRARRAY_INITIALIZER;
    char *userpat = xstrdup("user.*");
    userpat[4] = jmap_namespace.hier_sep;
    strarray_append(&patterns, userpat);
    struct findaccounts_data ctx = { accounts, BUF_INITIALIZER, 0, 0, 0, 0 };
    int r = mboxlist_findallmulti(&jmap_namespace, &patterns, 0, userid,
                                  httpd_authstate, findaccounts_cb, &ctx);
    free(userpat);
    strarray_fini(&patterns);
    if (r) {
        syslog(LOG_ERR, "Can't determine shared JMAP accounts for user %s: %s",
                userid, error_message(r));
    }
    /* Finalise last seen account */
    findaccounts_add(&ctx);
    buf_free(&ctx.userid);

    char *inboxname = mboxname_user_mbox(userid, NULL);
    struct buf state = BUF_INITIALIZER;
    buf_printf(&state, MODSEQ_FMT, mboxname_readraclmodseq(inboxname));
    free(inboxname);

    json_t *jsettings = json_pack("{s:s s:o s:O s:s s:s s:s s:s}",
            "username", userid,
            "accounts", accounts,
            "capabilities", my_jmap_settings.capabilities,
            "apiUrl", JMAP_BASE_URL,
            "downloadUrl", JMAP_BASE_URL JMAP_DOWNLOAD_COL JMAP_DOWNLOAD_TPL,
            /* FIXME eventSourceUrl */
            "uploadUrl", JMAP_BASE_URL JMAP_UPLOAD_COL JMAP_UPLOAD_TPL,
            "state", buf_cstring(&state));

    buf_free(&state);
    return jsettings;
}

/* Handle a GET on the settings endpoint */
static int jmap_settings(struct transaction_t *txn)
{
    assert(httpd_userid);

    if (!my_jmap_settings.capabilities) {
        jmap_core_capabilities();
        jmap_user_capabilities(&my_jmap_settings);
        jmap_mail_capabilities(&my_jmap_settings);
        jmap_contact_capabilities(&my_jmap_settings);
        jmap_calendar_capabilities(&my_jmap_settings);
    }

    /* Create the response object */
    json_t *res = user_settings(httpd_userid);
    if (!res) {
        syslog(LOG_ERR, "JMAP auth: cannot determine user settings for %s",
                httpd_userid);
        return HTTP_SERVER_ERROR;
    }

    /* Response should not be cached */
    txn->flags.cc |= CC_NOCACHE | CC_NOSTORE | CC_REVALIDATE;

    /* Write the JSON response */
    return json_response(HTTP_OK, txn, res);
}


/*
 * JMAP Core API Methods
 */

/* Core/echo method */
static int jmap_core_echo(jmap_req_t *req)
{
    json_array_append_new(req->response,
                          json_pack("[s,O,s]", "Core/echo", req->args, req->tag));
    return 0;
}

static int jmap_copyblob(jmap_req_t *req,
                         const char *blobid,
                         const char *from_accountid,
                         struct mailbox *to_mbox)
{
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    struct body *body = NULL;
    const struct body *part = NULL;
    struct buf msg_buf = BUF_INITIALIZER;
    FILE *to_fp = NULL;
    struct stagemsg *stage = NULL;

    int r = jmap_findblob(req, from_accountid, blobid,
                          &mbox, &mr, &body, &part, &msg_buf);
    if (r) return r;

    if (!part)
        part = body;

    if (!buf_base(&msg_buf)) {
        /* Map the message into memory */
        r = msgrecord_get_body(mr, &msg_buf);
        if (r) {
            syslog(LOG_ERR, "jmap_copyblob(%s): msgrecord_get_body: %s",
                   blobid, error_message(r));
            goto done;
        }
    }

    /* Create staging file */
    time_t internaldate = time(NULL);
    if (!(to_fp = append_newstage(to_mbox->name, internaldate, 0, &stage))) {
        syslog(LOG_ERR, "jmap_copyblob(%s): append_newstage(%s) failed",
                blobid, mbox->name);
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Copy blob. Keep the original MIME headers, we wouldn't really
     * know which ones are safe to rewrite for arbitrary blobs. */
    fwrite(buf_base(&msg_buf) + part->header_offset,
           part->header_size + part->content_size, 1, to_fp);
    if (ferror(to_fp)) {
        syslog(LOG_ERR, "jmap_copyblob(%s): tofp=%s: %s",
               blobid, append_stagefname(stage), strerror(errno));
        r = IMAP_IOERROR;
        goto done;
    }
    fclose(to_fp);
    to_fp = NULL;

    /* Append blob to mailbox */
    struct body *to_body = NULL;
    struct appendstate as;
    r = append_setup_mbox(&as, to_mbox, httpd_userid, httpd_authstate,
            0, /*quota*/NULL, 0, 0, /*event*/0);
    if (r) {
        syslog(LOG_ERR, "jmap_copyblob(%s): append_setup_mbox: %s",
                blobid, error_message(r));
        goto done;
    }
    strarray_t flags = STRARRAY_INITIALIZER;
    strarray_append(&flags, "\\Deleted");
    strarray_append(&flags, "\\Expunged");  // custom flag to insta-expunge!
	r = append_fromstage(&as, &to_body, stage, 0, internaldate, &flags, 0, NULL);
    strarray_fini(&flags);
	if (r) {
        syslog(LOG_ERR, "jmap_copyblob(%s): append_fromstage: %s",
                blobid, error_message(r));
		append_abort(&as);
		goto done;
	}
	message_free_body(to_body);
	free(to_body);
	r = append_commit(&as);
	if (r) {
        syslog(LOG_ERR, "jmap_copyblob(%s): append_commit: %s",
                blobid, error_message(r));
        goto done;
    }

done:
    if (stage) append_removestage(stage);
    if (to_fp) fclose(to_fp);
    buf_free(&msg_buf);
    message_free_body(body);
    free(body);
    msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    return r;
}

/* Blob/copy method */
static int jmap_blob_copy(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_copy copy;
    json_t *val, *err = NULL;
    size_t i = 0;
    int r = 0;
    struct mailbox *to_mbox = NULL;

    /* Parse request */
    jmap_copy_parse(req->args, &parser, req, NULL, &copy, &err);
    if (err) {
        jmap_error(req, err);
        goto cleanup;
    }

    /* Check if we can upload to toAccountId */
    r = create_upload_collection(req->accountid, &to_mbox);
    if (r == IMAP_PERMISSION_DENIED) {
        json_array_foreach(copy.create, i, val) {
            json_object_set(copy.not_created, json_string_value(val),
                    json_pack("{s:s}", "type", "toAccountNotFound"));
        }
        goto done;
    } else if (r) {
        syslog(LOG_ERR, "jmap_blob_copy: create_upload_collection(%s): %s",
               req->accountid, error_message(r));
        goto cleanup;
    }

    /* Copy blobs one by one. XXX should we batch copy here? */
    json_array_foreach(copy.create, i, val) {
        const char *blobid = json_string_value(val);
        r = jmap_copyblob(req, blobid, copy.from_account_id, to_mbox);
        if (r == IMAP_NOTFOUND || r == IMAP_PERMISSION_DENIED) {
            json_object_set_new(copy.not_created, blobid,
                    json_pack("{s:s}", "type", "blobNotFound"));
        }
        else if (r) goto cleanup;
        else json_object_set_new(copy.created, blobid, json_string(blobid));
    }

done:
    /* Build response */
    jmap_ok(req, jmap_copy_reply(&copy));
    r = 0;

cleanup:
    jmap_parser_fini(&parser);
    jmap_copy_fini(&copy);
    mailbox_close(&to_mbox);
    return r;
}

/* Blob/get method */

struct getblob_rec {
    const char *blob_id;
    uint32_t uid;
    char *part;
};

struct getblob_cb_rock {
    jmap_req_t *req;
    const char *blob_id;
    hash_table *getblobs_by_mboxid;
};

static int getblob_cb(const conv_guidrec_t* rec, void* vrock)
{
    struct getblob_cb_rock *rock = vrock;

    struct getblob_rec *getblob = xzmalloc(sizeof(struct getblob_rec));
    getblob->blob_id = rock->blob_id;
    getblob->uid = rec->uid;
    getblob->part = xstrdupnull(rec->part);

    ptrarray_t *getblobs = hash_lookup(rec->mboxid, rock->getblobs_by_mboxid);
    if (!getblobs) {
        getblobs = ptrarray_new();
        hash_insert(rec->mboxid, getblobs, rock->getblobs_by_mboxid);
    }
    ptrarray_append(getblobs, getblob);

    return 0;
}

static const jmap_property_t blob_props[] = {
    { "mailboxIds",      JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE },
    { "threadIds",       JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE },
    { "emailIds",        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE },
    { NULL,             0 }
};

static int jmap_blob_get(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_get get;
    json_t *err = NULL;
    json_t *jval;
    size_t i;

    /* Parse request */
    jmap_get_parse(req->args, &parser, req, blob_props, NULL, NULL, &get, 0, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Sort blob lookups by mailbox */
    hash_table getblobs_by_mboxid = HASH_TABLE_INITIALIZER;
    construct_hash_table(&getblobs_by_mboxid, 128, 0);
    json_array_foreach(get.ids, i, jval) {
        const char *blob_id = json_string_value(jval);
        if (*blob_id == 'G') {
            struct getblob_cb_rock rock = { req, blob_id, &getblobs_by_mboxid };
            int r = conversations_guid_foreach(req->cstate, blob_id + 1, getblob_cb, &rock);
            if (r) {
                syslog(LOG_ERR, "jmap_blob_get: can't lookup guid %s: %s",
                        blob_id, error_message(r));
            }
        }
    }

    /* Lookup blobs by mailbox */
    json_t *found = json_object();
    hash_iter *iter = hash_table_iter(&getblobs_by_mboxid);
    while (hash_iter_next(iter)) {
        const char *mboxid = hash_iter_key(iter);
        ptrarray_t *getblobs = hash_iter_val(iter);
        mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, mboxid, 0);
        struct mailbox *mbox = NULL;

        /* Open mailbox */
        if (!mbentry || !jmap_hasrights(req, mbentry, ACL_READ|ACL_LOOKUP)) {
            mboxlist_entry_free(&mbentry);
            continue;
        }
        int r = jmap_openmbox(req, mbentry->name, &mbox, 0);
        if (r) {
            syslog(LOG_ERR, "jmap_blob_get: can't open mailbox %s: %s",
                    mbentry->name, error_message(r));
            mboxlist_entry_free(&mbentry);
            continue;
        }
        mboxlist_entry_free(&mbentry);

        int j;
        for (j = 0; j < ptrarray_size(getblobs); j++) {
            struct getblob_rec *getblob = ptrarray_nth(getblobs, j);

            /* Read message record */
            struct message_guid guid;
            bit64 cid;
            msgrecord_t *mr = NULL;
            r = msgrecord_find(mbox, getblob->uid, &mr);
            if (!r) r = msgrecord_get_guid(mr, &guid);
            if (!r) r = msgrecord_get_cid(mr, &cid);
            msgrecord_unref(&mr);
            if (r) {
                syslog(LOG_ERR, "jmap_blob_get: can't read msgrecord %s:%d: %s",
                        mbox->name, getblob->uid, error_message(r));
                continue;
            }

            /* Report Blob entry */
            json_t *jblob = json_object_get(found, getblob->blob_id);
            if (!jblob) {
                jblob = json_object();
                json_object_set_new(found, getblob->blob_id, jblob);
            }
            if (jmap_wantprop(get.props, "mailboxIds")) {
                json_t *jmailboxIds = json_object_get(jblob, "mailboxIds");
                if (!jmailboxIds) {
                    jmailboxIds = json_object();
                    json_object_set_new(jblob, "mailboxIds", jmailboxIds);
                }
                json_object_set_new(jmailboxIds, mbox->uniqueid, json_true());
            }
            if (jmap_wantprop(get.props, "emailIds")) {
                json_t *jemailIds = json_object_get(jblob, "emailIds");
                if (!jemailIds) {
                    jemailIds = json_object();
                    json_object_set_new(jblob, "emailIds", jemailIds);
                }
                char emailid[JMAP_EMAILID_SIZE];
                jmap_set_emailid(&guid, emailid);
                json_object_set_new(jemailIds, emailid, json_true());
            }
            if (jmap_wantprop(get.props, "threadIds")) {
                json_t *jthreadIds = json_object_get(jblob, "threadIds");
                if (!jthreadIds) {
                    jthreadIds = json_object();
                    json_object_set_new(jblob, "threadIds", jthreadIds);
                }
                char threadid[JMAP_THREADID_SIZE];
                jmap_set_threadid(cid, threadid);
                json_object_set_new(jthreadIds, threadid, json_true());
            }
        }

       jmap_closembox(req, &mbox);
    }

    /* Clean up memory */
    hash_iter_reset(iter);
    while (hash_iter_next(iter)) {
        ptrarray_t *getblobs = hash_iter_val(iter);
        struct getblob_rec *getblob;
        while ((getblob = ptrarray_pop(getblobs))) {
            free(getblob->part);
            free(getblob);
        }
        ptrarray_free(getblobs);
    }
    hash_iter_free(&iter);
    free_hash_table(&getblobs_by_mboxid, NULL);

    /* Report found blobs */
    if (json_object_size(found)) {
        const char *blob_id;
        json_t *jblob;
        json_object_foreach(found, blob_id, jblob) {
            json_array_append(get.list, jblob);
        }
    }

    /* Report unknown or erroneous blobs */
    json_array_foreach(get.ids, i, jval) {
        const char *blob_id = json_string_value(jval);
        if (!json_object_get(found, blob_id)) {
            json_array_append_new(get.not_found, json_string(blob_id));
        }
    }

    json_decref(found);

    /* Reply */
    jmap_ok(req, jmap_get_reply(&get));

done:
    jmap_parser_fini(&parser);
    jmap_get_fini(&get);
    return 0;
}


/*
 * WebSockets data callback ('jmap' sub-protocol): Process JMAP API request.
 *
 * Can be tested with:
 *   https://github.com/websockets/wscat
 *   https://chrome.google.com/webstore/detail/web-socket-client/lifhekgaodigcpmnakfhaaaboididbdn
 *
 * WebSockets over HTTP/2 currently only available in:
 *   https://www.google.com/chrome/browser/canary.html
 */
static int jmap_ws(struct buf *inbuf, struct buf *outbuf,
                   struct buf *logbuf, void **rock)
{
    struct transaction_t **txnp = (struct transaction_t **) rock;
    struct transaction_t *txn = *txnp;
    json_t *res = NULL;
    int ret;

    if (!txn) {
        /* Create a transaction rock to use for API requests */
        txn = *txnp = xzmalloc(sizeof(struct transaction_t));
        txn->meth = METH_UNKNOWN;
        txn->req_body.flags = BODY_DONE;

        /* Create header cache */
        txn->req_hdrs = spool_new_hdrcache();
        if (!txn->req_hdrs) {
            free(txn);
            return HTTP_SERVER_ERROR;
        }

        /* Set Content-Type of request payload */
        spool_cache_header(xstrdup("Content-Type"),
                           xstrdup("application/json"), txn->req_hdrs);
    }
    else if (!inbuf) {
        /* Free transaction rock */
        transaction_free(txn);
        free(txn);
        return 0;
    }

    /* Set request payload */
    buf_init_ro(&txn->req_body.payload, buf_base(inbuf), buf_len(inbuf));

    /* Process the API request */
    ret = jmap_api(txn, &res, &my_jmap_settings);

    /* Free request payload */
    buf_free(&txn->req_body.payload);

    if (logbuf) {
        /* Log JMAP methods */
        const char **hdr = spool_getheader(txn->req_hdrs, ":jmap");

        if (hdr) buf_printf(logbuf, "; jmap=%s", hdr[0]);
    }

    if (!ret) {
        /* Return the JSON object */
        size_t flags = JSON_PRESERVE_ORDER;
        char *buf;

        /* Dump JSON object into a text buffer */
        flags |= (config_httpprettytelemetry ? JSON_INDENT(2) : JSON_COMPACT);
        buf = json_dumps(res, flags);
        json_decref(res);

        buf_initm(outbuf, buf, strlen(buf));
    }

    return ret;
}
