#include "polyfill.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* zlib.inflate(buffer) -> Promise<ArrayBuffer> */
static JSValue js_zlib_inflate(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "inflate requires 1 argument");

    size_t data_len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &data_len, argv[0]);
    const char *str = NULL;
    if (!data) {
        str = JS_ToCStringLen(ctx, &data_len, argv[0]);
        if (!str) return JS_EXCEPTION;
        data = (uint8_t *)str;
    }

    /* Create promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    /* Try zlib inflate with auto-detect (MAX_WBITS + 32) */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    int ret = inflateInit2(&zs, MAX_WBITS + 32);
    if (ret != Z_OK) {
        JSValue err = JS_NewString(ctx, "inflate: init failed");
        JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
        goto done;
    }

    zs.next_in = data;
    zs.avail_in = (uInt)data_len;

    {
        size_t out_cap = data_len * 4;
        if (out_cap < 1024) out_cap = 1024;
        uint8_t *out_buf = malloc(out_cap);
        size_t out_len = 0;

        do {
            if (out_len >= out_cap) {
                out_cap *= 2;
                out_buf = realloc(out_buf, out_cap);
            }
            zs.next_out = out_buf + out_len;
            zs.avail_out = (uInt)(out_cap - out_len);
            ret = inflate(&zs, Z_NO_FLUSH);
            out_len = out_cap - zs.avail_out;
        } while (ret == Z_OK);

        inflateEnd(&zs);

        if (ret == Z_STREAM_END) {
            JSValue buf = JS_NewArrayBufferCopy(ctx, out_buf, out_len);
            JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &buf);
            JS_FreeValue(ctx, buf);
        } else {
            char errmsg[128];
            snprintf(errmsg, sizeof(errmsg), "inflate error: %d", ret);
            JSValue err = JS_NewString(ctx, errmsg);
            JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
            JS_FreeValue(ctx, err);
        }
        free(out_buf);
    }

done:
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

/* zlib.deflate(buffer) -> Promise<ArrayBuffer> */
static JSValue js_zlib_deflate(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "deflate requires 1 argument");

    size_t data_len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &data_len, argv[0]);
    const char *str = NULL;
    if (!data) {
        str = JS_ToCStringLen(ctx, &data_len, argv[0]);
        if (!str) return JS_EXCEPTION;
        data = (uint8_t *)str;
    }

    /* Create promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    int ret = deflateInit(&zs, Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK) {
        JSValue err = JS_NewString(ctx, "deflate: init failed");
        JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
        goto done2;
    }

    zs.next_in = data;
    zs.avail_in = (uInt)data_len;

    {
        size_t out_cap = deflateBound(&zs, (uLong)data_len);
        uint8_t *out_buf = malloc(out_cap);
        zs.next_out = out_buf;
        zs.avail_out = (uInt)out_cap;

        ret = deflate(&zs, Z_FINISH);
        size_t out_len = out_cap - zs.avail_out;
        deflateEnd(&zs);

        if (ret == Z_STREAM_END) {
            JSValue buf = JS_NewArrayBufferCopy(ctx, out_buf, out_len);
            JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &buf);
            JS_FreeValue(ctx, buf);
        } else {
            char errmsg[128];
            snprintf(errmsg, sizeof(errmsg), "deflate error: %d", ret);
            JSValue err = JS_NewString(ctx, errmsg);
            JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
            JS_FreeValue(ctx, err);
        }
        free(out_buf);
    }

done2:
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

void polyfill_inject_zlib(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue zlibObj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, zlibObj, "inflate",
        JS_NewCFunction(ctx, js_zlib_inflate, "inflate", 1));
    JS_SetPropertyStr(ctx, zlibObj, "deflate",
        JS_NewCFunction(ctx, js_zlib_deflate, "deflate", 1));

    JS_SetPropertyStr(ctx, global, "zlib", zlibObj);
    JS_FreeValue(ctx, global);
}
