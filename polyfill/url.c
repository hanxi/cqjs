#include "polyfill.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---- Simple URL parser ---- */

typedef struct {
    char *scheme;    /* e.g. "https" */
    char *username;
    char *password;
    char *hostname;
    char *port;
    char *path;
    char *query;     /* without '?' */
    char *fragment;  /* without '#' */
} parsed_url_t;

static void parsed_url_free(parsed_url_t *u) {
    free(u->scheme); free(u->username); free(u->password);
    free(u->hostname); free(u->port); free(u->path);
    free(u->query); free(u->fragment);
}

static void parse_url(const char *url_str, parsed_url_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = url_str;

    /* scheme */
    const char *scheme_end = strstr(p, "://");
    if (scheme_end) {
        out->scheme = strndup(p, scheme_end - p);
        p = scheme_end + 3;
    } else {
        out->scheme = strdup("");
    }

    /* find end of authority */
    const char *path_start = strchr(p, '/');
    const char *query_start = strchr(p, '?');
    const char *frag_start = strchr(p, '#');
    const char *authority_end = path_start;
    if (!authority_end || (query_start && query_start < authority_end))
        authority_end = query_start;
    if (!authority_end || (frag_start && frag_start < authority_end))
        authority_end = frag_start;
    if (!authority_end)
        authority_end = p + strlen(p);

    /* extract authority */
    char *authority = strndup(p, authority_end - p);
    p = authority_end;

    /* userinfo */
    char *at = strchr(authority, '@');
    char *host_part;
    if (at) {
        *at = '\0';
        char *colon = strchr(authority, ':');
        if (colon) {
            *colon = '\0';
            out->username = strdup(authority);
            out->password = strdup(colon + 1);
        } else {
            out->username = strdup(authority);
            out->password = strdup("");
        }
        host_part = at + 1;
    } else {
        out->username = strdup("");
        out->password = strdup("");
        host_part = authority;
    }

    /* hostname:port */
    char *port_sep = strrchr(host_part, ':');
    if (port_sep && strchr(host_part, '[') == NULL) {
        *port_sep = '\0';
        out->hostname = strdup(host_part);
        out->port = strdup(port_sep + 1);
    } else {
        out->hostname = strdup(host_part);
        out->port = strdup("");
    }
    free(authority);

    /* path */
    if (*p == '/') {
        const char *end = p;
        while (*end && *end != '?' && *end != '#') end++;
        out->path = strndup(p, end - p);
        p = end;
    } else {
        out->path = strdup("/");
    }

    /* query */
    if (*p == '?') {
        p++;
        const char *end = strchr(p, '#');
        if (end) {
            out->query = strndup(p, end - p);
            p = end;
        } else {
            out->query = strdup(p);
            p += strlen(p);
        }
    } else {
        out->query = strdup("");
    }

    /* fragment */
    if (*p == '#') {
        p++;
        out->fragment = strdup(p);
    } else {
        out->fragment = strdup("");
    }
}

/* ---- URLSearchParams ---- */

typedef struct sp_entry {
    char *key;
    char *value;
    struct sp_entry *next;
} sp_entry_t;

typedef struct {
    sp_entry_t *head;
} search_params_t;

static search_params_t *sp_parse(const char *query) {
    search_params_t *sp = calloc(1, sizeof(search_params_t));
    if (!query || !*query) return sp;

    /* skip leading '?' */
    if (*query == '?') query++;

    char *dup = strdup(query);
    char *saveptr;
    char *pair = strtok_r(dup, "&", &saveptr);
    sp_entry_t **tail = &sp->head;
    while (pair) {
        char *eq = strchr(pair, '=');
        sp_entry_t *e = calloc(1, sizeof(sp_entry_t));
        if (eq) {
            e->key = strndup(pair, eq - pair);
            e->value = strdup(eq + 1);
        } else {
            e->key = strdup(pair);
            e->value = strdup("");
        }
        *tail = e;
        tail = &e->next;
        pair = strtok_r(NULL, "&", &saveptr);
    }
    free(dup);
    return sp;
}

/* Store sp pointer in a hidden property as int64 */
static search_params_t *get_sp_from_this(JSContext *ctx, JSValueConst this_val) {
    JSValue sp_val = JS_GetPropertyStr(ctx, this_val, "__sp_ptr");
    if (JS_IsUndefined(sp_val)) return NULL;
    int64_t ptr_val;
    JS_ToInt64(ctx, &ptr_val, sp_val);
    JS_FreeValue(ctx, sp_val);
    return (search_params_t *)(uintptr_t)ptr_val;
}

static JSValue js_sp_get(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    search_params_t *sp = get_sp_from_this(ctx, this_val);
    if (!sp) return JS_NULL;
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    for (sp_entry_t *e = sp->head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            JS_FreeCString(ctx, key);
            return JS_NewString(ctx, e->value);
        }
    }
    JS_FreeCString(ctx, key);
    return JS_NULL;
}

static JSValue js_sp_set(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    search_params_t *sp = get_sp_from_this(ctx, this_val);
    if (!sp) return JS_UNDEFINED;
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!key || !value) {
        if (key) JS_FreeCString(ctx, key);
        if (value) JS_FreeCString(ctx, value);
        return JS_UNDEFINED;
    }
    /* update existing or add new */
    for (sp_entry_t *e = sp->head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            e->value = strdup(value);
            JS_FreeCString(ctx, key);
            JS_FreeCString(ctx, value);
            return JS_UNDEFINED;
        }
    }
    sp_entry_t *e = calloc(1, sizeof(sp_entry_t));
    e->key = strdup(key);
    e->value = strdup(value);
    e->next = sp->head;
    sp->head = e;
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue js_sp_has(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewBool(ctx, 0);
    search_params_t *sp = get_sp_from_this(ctx, this_val);
    if (!sp) return JS_NewBool(ctx, 0);
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    for (sp_entry_t *e = sp->head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            JS_FreeCString(ctx, key);
            return JS_NewBool(ctx, 1);
        }
    }
    JS_FreeCString(ctx, key);
    return JS_NewBool(ctx, 0);
}

static JSValue js_sp_delete(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    search_params_t *sp = get_sp_from_this(ctx, this_val);
    if (!sp) return JS_UNDEFINED;
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    sp_entry_t **pp = &sp->head;
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            sp_entry_t *del = *pp;
            *pp = del->next;
            free(del->key); free(del->value); free(del);
        } else {
            pp = &(*pp)->next;
        }
    }
    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}

static JSValue js_sp_to_string(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    search_params_t *sp = get_sp_from_this(ctx, this_val);
    if (!sp || !sp->head) return JS_NewString(ctx, "");
    /* build query string */
    size_t cap = 256;
    char *buf = malloc(cap);
    size_t len = 0;
    buf[0] = '\0';
    for (sp_entry_t *e = sp->head; e; e = e->next) {
        size_t need = strlen(e->key) + strlen(e->value) + 3;
        while (len + need >= cap) { cap *= 2; buf = realloc(buf, cap); }
        if (len > 0) buf[len++] = '&';
        len += sprintf(buf + len, "%s=%s", e->key, e->value);
    }
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

static JSValue js_sp_append(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    search_params_t *sp = get_sp_from_this(ctx, this_val);
    if (!sp) return JS_UNDEFINED;
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!key || !value) {
        if (key) JS_FreeCString(ctx, key);
        if (value) JS_FreeCString(ctx, value);
        return JS_UNDEFINED;
    }
    sp_entry_t *e = calloc(1, sizeof(sp_entry_t));
    e->key = strdup(key);
    e->value = strdup(value);
    /* append to end */
    sp_entry_t **tail = &sp->head;
    while (*tail) tail = &(*tail)->next;
    *tail = e;
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue build_search_params_obj(JSContext *ctx, search_params_t *sp) {
    JSValue obj = JS_NewObject(ctx);
    /* Store pointer as int64 */
    JS_SetPropertyStr(ctx, obj, "__sp_ptr",
        JS_NewInt64(ctx, (int64_t)(uintptr_t)sp));
    JS_SetPropertyStr(ctx, obj, "get",
        JS_NewCFunction(ctx, js_sp_get, "get", 1));
    JS_SetPropertyStr(ctx, obj, "set",
        JS_NewCFunction(ctx, js_sp_set, "set", 2));
    JS_SetPropertyStr(ctx, obj, "has",
        JS_NewCFunction(ctx, js_sp_has, "has", 1));
    JS_SetPropertyStr(ctx, obj, "delete",
        JS_NewCFunction(ctx, js_sp_delete, "delete", 1));
    JS_SetPropertyStr(ctx, obj, "toString",
        JS_NewCFunction(ctx, js_sp_to_string, "toString", 0));
    JS_SetPropertyStr(ctx, obj, "append",
        JS_NewCFunction(ctx, js_sp_append, "append", 2));
    return obj;
}

/* ---- URL toString helper ---- */
static JSValue js_url_to_string(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    JSValue href = JS_GetPropertyStr(ctx, this_val, "href");
    return href;
}

/* ---- URL constructor ---- */
static JSValue js_url_constructor(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "URL requires at least 1 argument");
    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_EXCEPTION;

    parsed_url_t parsed;
    parse_url(url_str, &parsed);
    JS_FreeCString(ctx, url_str);

    JSValue obj = JS_NewObject(ctx);

    /* Build href */
    char href[8192];
    snprintf(href, sizeof(href), "%s://%s%s%s%s%s%s%s%s",
        parsed.scheme,
        parsed.hostname,
        parsed.port[0] ? ":" : "", parsed.port,
        parsed.path,
        parsed.query[0] ? "?" : "", parsed.query,
        parsed.fragment[0] ? "#" : "", parsed.fragment);
    JS_SetPropertyStr(ctx, obj, "href", JS_NewString(ctx, href));

    char protocol[64];
    snprintf(protocol, sizeof(protocol), "%s:", parsed.scheme);
    JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, protocol));

    char host[512];
    if (parsed.port[0]) {
        snprintf(host, sizeof(host), "%s:%s", parsed.hostname, parsed.port);
    } else {
        snprintf(host, sizeof(host), "%s", parsed.hostname);
    }
    JS_SetPropertyStr(ctx, obj, "host", JS_NewString(ctx, host));
    JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, parsed.hostname));
    JS_SetPropertyStr(ctx, obj, "port", JS_NewString(ctx, parsed.port));
    JS_SetPropertyStr(ctx, obj, "pathname", JS_NewString(ctx, parsed.path));

    char search[4096];
    if (parsed.query[0]) {
        snprintf(search, sizeof(search), "?%s", parsed.query);
    } else {
        search[0] = '\0';
    }
    JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, search));

    char hash[4096];
    if (parsed.fragment[0]) {
        snprintf(hash, sizeof(hash), "#%s", parsed.fragment);
    } else {
        hash[0] = '\0';
    }
    JS_SetPropertyStr(ctx, obj, "hash", JS_NewString(ctx, hash));

    char origin[1024];
    snprintf(origin, sizeof(origin), "%s://%s", parsed.scheme, host);
    JS_SetPropertyStr(ctx, obj, "origin", JS_NewString(ctx, origin));
    JS_SetPropertyStr(ctx, obj, "username", JS_NewString(ctx, parsed.username));
    JS_SetPropertyStr(ctx, obj, "password", JS_NewString(ctx, parsed.password));

    /* searchParams */
    search_params_t *sp = sp_parse(parsed.query);
    JSValue sp_obj = build_search_params_obj(ctx, sp);
    JS_SetPropertyStr(ctx, obj, "searchParams", sp_obj);

    /* toString method */
    JS_SetPropertyStr(ctx, obj, "toString",
        JS_NewCFunction(ctx, js_url_to_string, "toString", 0));

    parsed_url_free(&parsed);
    return obj;
}

/* URLSearchParams constructor */
static JSValue js_urlsearchparams_constructor(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    const char *query = "";
    const char *query_cstr = NULL;
    if (argc > 0) {
        query_cstr = JS_ToCString(ctx, argv[0]);
        if (!query_cstr) return JS_EXCEPTION;
        query = query_cstr;
    }
    search_params_t *sp = sp_parse(query);
    if (query_cstr) JS_FreeCString(ctx, query_cstr);
    return build_search_params_obj(ctx, sp);
}

void polyfill_inject_url(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "URL",
        JS_NewCFunction(ctx, js_url_constructor, "URL", 2));
    JS_SetPropertyStr(ctx, global, "URLSearchParams",
        JS_NewCFunction(ctx, js_urlsearchparams_constructor, "URLSearchParams", 1));
    JS_FreeValue(ctx, global);
}
