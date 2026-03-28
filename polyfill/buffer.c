#include "polyfill.h"
#include <stdlib.h>
#include <string.h>

/* Buffer.from(data, encoding) */
static JSValue js_buffer_from(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArrayBufferCopy(ctx, NULL, 0);

    /* Check if first arg is already an ArrayBuffer */
    size_t buf_len;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, argv[0]);
    if (buf) {
        return JS_NewArrayBufferCopy(ctx, buf, buf_len);
    }

    /* String input */
    size_t str_len;
    const char *str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str) return JS_EXCEPTION;

    const char *encoding = "utf8";
    const char *enc_cstr = NULL;
    if (argc > 1) {
        enc_cstr = JS_ToCString(ctx, argv[1]);
        if (enc_cstr) encoding = enc_cstr;
    }

    JSValue result;
    if (strcmp(encoding, "base64") == 0) {
        size_t decoded_len;
        uint8_t *decoded = base64_decode(str, str_len, &decoded_len);
        result = decoded ? JS_NewArrayBufferCopy(ctx, decoded, decoded_len)
                         : JS_NewArrayBufferCopy(ctx, NULL, 0);
        free(decoded);
    } else if (strcmp(encoding, "hex") == 0) {
        size_t decoded_len;
        uint8_t *decoded = hex_decode(str, str_len, &decoded_len);
        result = decoded ? JS_NewArrayBufferCopy(ctx, decoded, decoded_len)
                         : JS_NewArrayBufferCopy(ctx, NULL, 0);
        free(decoded);
    } else {
        /* utf8, utf-8, ascii, latin1, binary */
        result = JS_NewArrayBufferCopy(ctx, (const uint8_t *)str, str_len);
    }

    JS_FreeCString(ctx, str);
    if (enc_cstr) JS_FreeCString(ctx, enc_cstr);
    return result;
}

/* Buffer.isBuffer(obj) */
static JSValue js_buffer_is_buffer(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewBool(ctx, 0);
    size_t buf_len;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, argv[0]);
    return JS_NewBool(ctx, buf != NULL);
}

/* Buffer.toString(buf, encoding) */
static JSValue js_buffer_to_string(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewString(ctx, "");

    size_t buf_len;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, argv[0]);
    if (!buf) {
        /* fallback: try as string */
        const char *str = JS_ToCString(ctx, argv[0]);
        if (!str) return JS_NewString(ctx, "");
        JSValue result = JS_NewString(ctx, str);
        JS_FreeCString(ctx, str);
        return result;
    }

    const char *encoding = "utf8";
    const char *enc_cstr = NULL;
    if (argc > 1) {
        enc_cstr = JS_ToCString(ctx, argv[1]);
        if (enc_cstr) encoding = enc_cstr;
    }

    JSValue result;
    if (strcmp(encoding, "base64") == 0) {
        size_t encoded_len;
        char *encoded = base64_encode(buf, buf_len, &encoded_len);
        result = encoded ? JS_NewStringLen(ctx, encoded, encoded_len)
                         : JS_NewString(ctx, "");
        free(encoded);
    } else if (strcmp(encoding, "hex") == 0) {
        size_t encoded_len;
        char *encoded = hex_encode(buf, buf_len, &encoded_len);
        result = encoded ? JS_NewStringLen(ctx, encoded, encoded_len)
                         : JS_NewString(ctx, "");
        free(encoded);
    } else {
        /* utf8, utf-8, ascii */
        result = JS_NewStringLen(ctx, (const char *)buf, buf_len);
    }

    if (enc_cstr) JS_FreeCString(ctx, enc_cstr);
    return result;
}

void polyfill_inject_buffer(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue buffer = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, buffer, "from",
        JS_NewCFunction(ctx, js_buffer_from, "from", 2));
    JS_SetPropertyStr(ctx, buffer, "isBuffer",
        JS_NewCFunction(ctx, js_buffer_is_buffer, "isBuffer", 1));
    JS_SetPropertyStr(ctx, buffer, "toString",
        JS_NewCFunction(ctx, js_buffer_to_string, "toString", 2));

    JS_SetPropertyStr(ctx, global, "Buffer", buffer);
    JS_FreeValue(ctx, global);
}
