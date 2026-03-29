#include "polyfill.h"
#include "../cqjs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <curl/curl.h>

/* ---- Fetch Manager implementation ---- */

void fm_init(fetch_manager_t *fm) {
    memset(fm, 0, sizeof(*fm));
    fm->head = NULL;
    fm->counter = 0;
    pipe(fm->notify_pipe);
    /* Set read end to non-blocking so drain loops don't block */
    fcntl(fm->notify_pipe[0], F_SETFL,
          fcntl(fm->notify_pipe[0], F_GETFL) | O_NONBLOCK);
    pthread_mutex_init(&fm->mutex, NULL);
    curl_global_init(CURL_GLOBAL_ALL);
}

/* libcurl write callback */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    curl_buf_t *buf = (curl_buf_t *)userdata;
    size_t total = size * nmemb;
    while (buf->len + total >= buf->cap) {
        buf->cap = buf->cap ? buf->cap * 2 : 4096;
        buf->data = realloc(buf->data, buf->cap);
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    return total;
}

/* libcurl header callback */
typedef struct {
    header_pair_t *headers;
    int count;
    int cap;
} header_buf_t;

static size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    header_buf_t *hb = (header_buf_t *)userdata;
    size_t total = size * nitems;

    /* skip status line and empty lines */
    if (total <= 2) return total;
    if (strncmp(buffer, "HTTP/", 5) == 0) return total;

    /* find colon */
    char *colon = memchr(buffer, ':', total);
    if (!colon) return total;

    size_t key_len = colon - buffer;
    char *key = strndup(buffer, key_len);
    /* lowercase key */
    for (size_t i = 0; i < key_len; i++) {
        if (key[i] >= 'A' && key[i] <= 'Z') key[i] += 32;
    }

    /* skip ": " */
    char *val_start = colon + 1;
    while (*val_start == ' ') val_start++;
    size_t val_len = total - (val_start - buffer);
    /* trim trailing \r\n */
    while (val_len > 0 && (val_start[val_len-1] == '\r' || val_start[val_len-1] == '\n'))
        val_len--;
    char *value = strndup(val_start, val_len);

    if (hb->count >= hb->cap) {
        hb->cap = hb->cap ? hb->cap * 2 : 16;
        hb->headers = realloc(hb->headers, hb->cap * sizeof(header_pair_t));
    }
    hb->headers[hb->count].key = key;
    hb->headers[hb->count].value = value;
    hb->count++;

    return total;
}

/* Worker thread arg - passes notify_fd alongside entry */
typedef struct {
    fetch_entry_t *entry;
    int notify_fd;
} fetch_worker_arg_t;

static void *fetch_worker_wrapper(void *arg) {
    fetch_worker_arg_t *wa = (fetch_worker_arg_t *)arg;
    fetch_entry_t *entry = wa->entry;
    int notify_fd = wa->notify_fd;
    free(wa);

    fprintf(stderr, "[DEBUG] fetch_worker: starting request method=%s url=%s\n",
            entry->method ? entry->method : "GET", entry->url);
    fflush(stderr);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[DEBUG] fetch_worker: curl_easy_init failed url=%s\n", entry->url);
        fflush(stderr);
        entry->is_error = 1;
        entry->error_msg = strdup("fetch: curl_easy_init failed");
        entry->completed = 1;
        char c = 'f';
        write(notify_fd, &c, 1);
        return NULL;
    }

    curl_buf_t body_buf = {0};
    header_buf_t header_buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, entry->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cqjs/1.0");

    if (entry->method) {
        if (strcmp(entry->method, "POST") == 0) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else if (strcmp(entry->method, "PUT") == 0 ||
                   strcmp(entry->method, "DELETE") == 0 ||
                   strcmp(entry->method, "PATCH") == 0) {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, entry->method);
        } else if (strcmp(entry->method, "HEAD") == 0) {
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        }
    }

    if (entry->body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, entry->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(entry->body));
    }

    struct curl_slist *slist = NULL;
    for (int i = 0; i < entry->header_count; i++) {
        char hdr[4096];
        snprintf(hdr, sizeof(hdr), "%s: %s",
                 entry->headers[i].key, entry->headers[i].value);
        slist = curl_slist_append(slist, hdr);
    }
    if (slist) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
    }

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[DEBUG] fetch_worker: curl error url=%s code=%d err=%s\n",
                entry->url, (int)res, curl_easy_strerror(res));
        fflush(stderr);
        entry->is_error = 1;
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "fetch error: %s", curl_easy_strerror(res));
        entry->error_msg = strdup(errmsg);
    } else {
        long status;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        entry->status_code = (int)status;

        fprintf(stderr, "[DEBUG] fetch_worker: success url=%s status=%ld body_len=%zu\n",
                entry->url, status, body_buf.len);
        // 打印 body 内容
        for (size_t i = 0; i < body_buf.len; i++) {
            fprintf(stderr, "%c", body_buf.data[i]);
        }
        fflush(stderr);

        char status_text[64];
        snprintf(status_text, sizeof(status_text), "%d", (int)status);
        entry->status_text = strdup(status_text);

        entry->response_body = body_buf.data;
        entry->response_body_len = body_buf.len;
        body_buf.data = NULL;

        entry->response_headers = header_buf.headers;
        entry->response_header_count = header_buf.count;
        header_buf.headers = NULL;
    }

    if (slist) curl_slist_free_all(slist);
    curl_easy_cleanup(curl);
    free(body_buf.data);
    free(header_buf.headers);

    entry->completed = 1;
    char c = 'f';
    write(notify_fd, &c, 1);

    return NULL;
}

/* ---- Response object builder ---- */

typedef struct {
    char *body;
    size_t body_len;
} resp_body_data_t;

static JSValue js_resp_text(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    JSValue body_val = JS_GetPropertyStr(ctx, this_val, "__body");
    const char *body = JS_ToCString(ctx, body_val);
    JS_FreeValue(ctx, body_val);

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    JSValue text = body ? JS_NewString(ctx, body) : JS_NewString(ctx, "");
    JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &text);
    JS_FreeValue(ctx, text);
    if (body) JS_FreeCString(ctx, body);

    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue js_resp_json(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    JSValue body_val = JS_GetPropertyStr(ctx, this_val, "__body");
    const char *body = JS_ToCString(ctx, body_val);
    JS_FreeValue(ctx, body_val);

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    if (body) {
        JSValue parsed = JS_ParseJSON(ctx, body, strlen(body), "<json>");
        if (JS_IsException(parsed)) {
            JSValue err = JS_NewString(ctx, "json parse error");
            JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
            JS_FreeValue(ctx, err);
            /* clear exception */
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
        } else {
            JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &parsed);
            JS_FreeValue(ctx, parsed);
        }
        JS_FreeCString(ctx, body);
    } else {
        JSValue err = JS_NewString(ctx, "empty body");
        JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
    }

    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue js_resp_array_buffer(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    JSValue body_val = JS_GetPropertyStr(ctx, this_val, "__body");
    size_t body_len;
    const char *body = JS_ToCStringLen(ctx, &body_len, body_val);
    JS_FreeValue(ctx, body_val);

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    JSValue buf = body ? JS_NewArrayBufferCopy(ctx, (const uint8_t *)body, body_len)
                       : JS_NewArrayBufferCopy(ctx, NULL, 0);
    JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &buf);
    JS_FreeValue(ctx, buf);
    if (body) JS_FreeCString(ctx, body);

    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

/* headers.get() helper */
static JSValue js_headers_get(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    /* lowercase the key */
    size_t klen = strlen(key);
    char *lkey = malloc(klen + 1);
    for (size_t i = 0; i < klen; i++) {
        lkey[i] = (key[i] >= 'A' && key[i] <= 'Z') ? key[i] + 32 : key[i];
    }
    lkey[klen] = '\0';
    JS_FreeCString(ctx, key);

    JSValue val = JS_GetPropertyStr(ctx, this_val, lkey);
    free(lkey);
    if (JS_IsUndefined(val)) {
        JS_FreeValue(ctx, val);
        return JS_NULL;
    }
    return val;
}

static JSValue js_headers_has(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewBool(ctx, 0);
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    size_t klen = strlen(key);
    char *lkey = malloc(klen + 1);
    for (size_t i = 0; i < klen; i++) {
        lkey[i] = (key[i] >= 'A' && key[i] <= 'Z') ? key[i] + 32 : key[i];
    }
    lkey[klen] = '\0';
    JS_FreeCString(ctx, key);

    JSValue val = JS_GetPropertyStr(ctx, this_val, lkey);
    free(lkey);
    int has = !JS_IsUndefined(val);
    JS_FreeValue(ctx, val);
    return JS_NewBool(ctx, has);
}

static JSValue build_response_object(JSContext *ctx, fetch_entry_t *entry) {
    JSValue resp = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, resp, "ok",
        JS_NewBool(ctx, entry->status_code >= 200 && entry->status_code < 300));
    JS_SetPropertyStr(ctx, resp, "status",
        JS_NewInt32(ctx, entry->status_code));
    JS_SetPropertyStr(ctx, resp, "statusText",
        JS_NewString(ctx, entry->status_text ? entry->status_text : ""));
    JS_SetPropertyStr(ctx, resp, "url",
        JS_NewString(ctx, entry->url ? entry->url : ""));

    /* Headers object */
    JSValue headers = JS_NewObject(ctx);
    for (int i = 0; i < entry->response_header_count; i++) {
        JS_SetPropertyStr(ctx, headers, entry->response_headers[i].key,
            JS_NewString(ctx, entry->response_headers[i].value));
    }
    JS_SetPropertyStr(ctx, headers, "get",
        JS_NewCFunction(ctx, js_headers_get, "get", 1));
    JS_SetPropertyStr(ctx, headers, "has",
        JS_NewCFunction(ctx, js_headers_has, "has", 1));
    JS_SetPropertyStr(ctx, resp, "headers", headers);

    /* Store body for text()/json()/arrayBuffer() */
    if (entry->response_body && entry->response_body_len > 0) {
        JS_SetPropertyStr(ctx, resp, "__body",
            JS_NewStringLen(ctx, entry->response_body, entry->response_body_len));
    } else {
        JS_SetPropertyStr(ctx, resp, "__body", JS_NewString(ctx, ""));
    }

    JS_SetPropertyStr(ctx, resp, "text",
        JS_NewCFunction(ctx, js_resp_text, "text", 0));
    JS_SetPropertyStr(ctx, resp, "json",
        JS_NewCFunction(ctx, js_resp_json, "json", 0));
    JS_SetPropertyStr(ctx, resp, "arrayBuffer",
        JS_NewCFunction(ctx, js_resp_array_buffer, "arrayBuffer", 0));

    return resp;
}

/* ---- Process pending fetches ---- */

int fm_process_pending(fetch_manager_t *fm, JSContext *ctx) {
    /* drain notification pipe */
    char buf[64];
    while (read(fm->notify_pipe[0], buf, sizeof(buf)) > 0) {}

    pthread_mutex_lock(&fm->mutex);
    if (!fm->head) {
        pthread_mutex_unlock(&fm->mutex);
        return 0;
    }

    fprintf(stderr, "[DEBUG] fm_process_pending: has entries, checking completion\n");
    fflush(stderr);

    /* Log all pending entries for debugging */
    {
        fetch_entry_t *dbg = fm->head;
        while (dbg) {
            fprintf(stderr, "[DEBUG] fm_process_pending: entry url=%s completed=%d is_error=%d\n",
                    dbg->url, dbg->completed, dbg->is_error);
            dbg = dbg->next;
        }
        fflush(stderr);
    }

    int processed = 0;
    fetch_entry_t **pp = &fm->head;
    while (*pp) {
        fetch_entry_t *e = *pp;
        if (e->completed) {
            fprintf(stderr, "[DEBUG] fm_process_pending: fetch completed url=%s is_error=%d status=%d\n",
                    e->url, e->is_error, e->status_code);
            fflush(stderr);
            *pp = e->next;
            pthread_mutex_unlock(&fm->mutex);

            /* resolve or reject on big stack to avoid stack overflow */
            if (e->is_error) {
                JSValue err = JS_NewString(ctx, e->error_msg ? e->error_msg : "fetch error");
                cqjs_call(ctx, e->reject, JS_UNDEFINED, 1, &err);
                JS_FreeValue(ctx, err);
            } else {
                JSValue resp = build_response_object(ctx, e);
                cqjs_call(ctx, e->resolve, JS_UNDEFINED, 1, &resp);
                JS_FreeValue(ctx, resp);
            }

            /* cleanup */
            JS_FreeValue(ctx, e->resolve);
            JS_FreeValue(ctx, e->reject);
            free(e->url);
            free(e->method);
            free(e->body);
            for (int i = 0; i < e->header_count; i++) {
                free(e->headers[i].key);
                free(e->headers[i].value);
            }
            free(e->headers);
            free(e->status_text);
            free(e->response_body);
            for (int i = 0; i < e->response_header_count; i++) {
                free(e->response_headers[i].key);
                free(e->response_headers[i].value);
            }
            free(e->response_headers);
            free(e->error_msg);
            free(e);

            processed++;
            pthread_mutex_lock(&fm->mutex);
            pp = &fm->head; /* restart */
            continue;
        }
        pp = &e->next;
    }

    pthread_mutex_unlock(&fm->mutex);
    return processed;
}

int fm_has_pending(fetch_manager_t *fm) {
    pthread_mutex_lock(&fm->mutex);
    int has = fm->head != NULL;
    pthread_mutex_unlock(&fm->mutex);
    return has;
}

void fm_free(fetch_manager_t *fm, JSContext *ctx) {
    pthread_mutex_lock(&fm->mutex);
    fetch_entry_t *e = fm->head;
    while (e) {
        fetch_entry_t *next = e->next;
        JS_FreeValue(ctx, e->resolve);
        JS_FreeValue(ctx, e->reject);
        free(e->url);
        free(e->method);
        free(e->body);
        for (int i = 0; i < e->header_count; i++) {
            free(e->headers[i].key);
            free(e->headers[i].value);
        }
        free(e->headers);
        free(e->status_text);
        free(e->response_body);
        for (int i = 0; i < e->response_header_count; i++) {
            free(e->response_headers[i].key);
            free(e->response_headers[i].value);
        }
        free(e->response_headers);
        free(e->error_msg);
        free(e);
        e = next;
    }
    fm->head = NULL;
    pthread_mutex_unlock(&fm->mutex);

    close(fm->notify_pipe[0]);
    close(fm->notify_pipe[1]);
    pthread_mutex_destroy(&fm->mutex);
    curl_global_cleanup();
}

/* ---- JS fetch() binding ---- */

static fetch_manager_t *get_fm(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, "__fm_ptr");
    JS_FreeValue(ctx, global);
    if (JS_IsUndefined(val)) return NULL;
    int64_t ptr;
    JS_ToInt64(ctx, &ptr, val);
    JS_FreeValue(ctx, val);
    return (fetch_manager_t *)(uintptr_t)ptr;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    fetch_manager_t *fm = get_fm(ctx);
    if (!fm || argc < 1) {
        return JS_ThrowTypeError(ctx, "fetch requires at least 1 argument");
    }

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    /* Parse options */
    char *method = strdup("GET");
    char *body = NULL;
    header_pair_t *headers = NULL;
    int header_count = 0;

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue opts = argv[1];

        JSValue method_val = JS_GetPropertyStr(ctx, opts, "method");
        if (!JS_IsUndefined(method_val) && !JS_IsNull(method_val)) {
            const char *m = JS_ToCString(ctx, method_val);
            if (m) {
                free(method);
                method = strdup(m);
                /* uppercase */
                for (char *p = method; *p; p++) {
                    if (*p >= 'a' && *p <= 'z') *p -= 32;
                }
                JS_FreeCString(ctx, m);
            }
        }
        JS_FreeValue(ctx, method_val);

        JSValue body_val = JS_GetPropertyStr(ctx, opts, "body");
        if (!JS_IsUndefined(body_val) && !JS_IsNull(body_val)) {
            const char *b = JS_ToCString(ctx, body_val);
            if (b) {
                body = strdup(b);
                JS_FreeCString(ctx, b);
            }
        }
        JS_FreeValue(ctx, body_val);

        JSValue headers_val = JS_GetPropertyStr(ctx, opts, "headers");
        if (JS_IsObject(headers_val)) {
            /* Get property names */
            JSPropertyEnum *tab;
            uint32_t len;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, headers_val,
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                headers = malloc(len * sizeof(header_pair_t));
                header_count = 0;
                for (uint32_t i = 0; i < len; i++) {
                    const char *key = JS_AtomToCString(ctx, tab[i].atom);
                    JSValue v = JS_GetProperty(ctx, headers_val, tab[i].atom);
                    const char *val = JS_ToCString(ctx, v);
                    if (key && val) {
                        headers[header_count].key = strdup(key);
                        headers[header_count].value = strdup(val);
                        header_count++;
                    }
                    if (key) JS_FreeCString(ctx, key);
                    if (val) JS_FreeCString(ctx, val);
                    JS_FreeValue(ctx, v);
                    JS_FreeAtom(ctx, tab[i].atom);
                }
                js_free(ctx, tab);
            }
        }
        JS_FreeValue(ctx, headers_val);
    }

    /* Create promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    /* Create fetch entry */
    fetch_entry_t *entry = calloc(1, sizeof(fetch_entry_t));
    entry->url = strdup(url);
    entry->method = method;
    entry->body = body;
    entry->headers = headers;
    entry->header_count = header_count;
    entry->resolve = resolving_funcs[0];
    entry->reject = resolving_funcs[1];
    entry->completed = 0;

    JS_FreeCString(ctx, url);

    fprintf(stderr, "[DEBUG] js_fetch: submitting fetch url=%s method=%s\n",
            entry->url, entry->method ? entry->method : "GET");
    fflush(stderr);

    /* Add to pending list */
    pthread_mutex_lock(&fm->mutex);
    entry->next = fm->head;
    fm->head = entry;
    pthread_mutex_unlock(&fm->mutex);

    /* Launch worker thread */
    fetch_worker_arg_t *wa = malloc(sizeof(fetch_worker_arg_t));
    wa->entry = entry;
    wa->notify_fd = fm->notify_pipe[1];

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, fetch_worker_wrapper, wa);
    pthread_attr_destroy(&attr);

    return promise;
}

void polyfill_inject_fetch(JSContext *ctx, fetch_manager_t *fm) {
    JSValue global = JS_GetGlobalObject(ctx);

    /* Store fm pointer */
    JS_SetPropertyStr(ctx, global, "__fm_ptr",
        JS_NewInt64(ctx, (int64_t)(uintptr_t)fm));

    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, js_fetch, "fetch", 2));

    JS_FreeValue(ctx, global);
}
