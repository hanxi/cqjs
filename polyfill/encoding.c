#include "polyfill.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Base64 encode/decode (RFC 4648) ---- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const unsigned char b64_decode_table[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
    ['4']=56,['5']=57,['6']=58,['7']=59,['+']=62,['/']=63,
};

/* Returns malloc'd base64 string. Caller must free. */
char *base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }
    /* padding */
    size_t mod = len % 3;
    if (mod == 1) { out[j-1] = '='; out[j-2] = '='; }
    else if (mod == 2) { out[j-1] = '='; }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

/* Returns malloc'd decoded data. Caller must free. */
uint8_t *base64_decode(const char *src, size_t src_len, size_t *out_len) {
    /* strip padding and whitespace */
    while (src_len > 0 && (src[src_len-1] == '=' ||
           src[src_len-1] == '\n' || src[src_len-1] == '\r'))
        src_len--;
    size_t olen = src_len * 3 / 4;
    uint8_t *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < src_len; ) {
        uint32_t a = i < src_len ? b64_decode_table[(unsigned char)src[i++]] : 0;
        uint32_t b = i < src_len ? b64_decode_table[(unsigned char)src[i++]] : 0;
        uint32_t c = i < src_len ? b64_decode_table[(unsigned char)src[i++]] : 0;
        uint32_t d = i < src_len ? b64_decode_table[(unsigned char)src[i++]] : 0;
        uint32_t triple = (a << 18) | (b << 12) | (c << 6) | d;
        if (j < olen) out[j++] = (triple >> 16) & 0xFF;
        if (j < olen) out[j++] = (triple >> 8) & 0xFF;
        if (j < olen) out[j++] = triple & 0xFF;
    }
    if (out_len) *out_len = j;
    return out;
}

/* ---- Hex encode/decode ---- */

char *hex_encode(const uint8_t *data, size_t len, size_t *out_len) {
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", data[i]);
    }
    out[len * 2] = '\0';
    if (out_len) *out_len = len * 2;
    return out;
}

uint8_t *hex_decode(const char *src, size_t src_len, size_t *out_len) {
    size_t olen = src_len / 2;
    uint8_t *out = malloc(olen);
    if (!out) return NULL;
    for (size_t i = 0; i < olen; i++) {
        unsigned int val;
        sscanf(src + i * 2, "%2x", &val);
        out[i] = (uint8_t)val;
    }
    if (out_len) *out_len = olen;
    return out;
}

/* ---- atob ---- */
static JSValue js_atob(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "atob requires 1 argument");
    const char *encoded = JS_ToCString(ctx, argv[0]);
    if (!encoded) return JS_EXCEPTION;
    size_t decoded_len;
    uint8_t *decoded = base64_decode(encoded, strlen(encoded), &decoded_len);
    JS_FreeCString(ctx, encoded);
    if (!decoded) return JS_ThrowTypeError(ctx, "atob: invalid base64 string");
    JSValue result = JS_NewStringLen(ctx, (const char *)decoded, decoded_len);
    free(decoded);
    return result;
}

/* ---- btoa ---- */
static JSValue js_btoa(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "btoa requires 1 argument");
    size_t str_len;
    const char *str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str) return JS_EXCEPTION;
    size_t encoded_len;
    char *encoded = base64_encode((const uint8_t *)str, str_len, &encoded_len);
    JS_FreeCString(ctx, str);
    if (!encoded) return JS_ThrowTypeError(ctx, "btoa: encoding failed");
    JSValue result = JS_NewStringLen(ctx, encoded, encoded_len);
    free(encoded);
    return result;
}

/* ---- TextEncoder.encode ---- */
static JSValue js_text_encoder_encode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArrayBufferCopy(ctx, NULL, 0);
    size_t str_len;
    const char *str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str) return JS_EXCEPTION;
    JSValue result = JS_NewArrayBufferCopy(ctx, (const uint8_t *)str, str_len);
    JS_FreeCString(ctx, str);
    return result;
}

/* ---- TextDecoder.decode ---- */
static JSValue js_text_decoder_decode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    size_t buf_len;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, argv[0]);
    if (buf) {
        return JS_NewStringLen(ctx, (const char *)buf, buf_len);
    }
    /* fallback: try as string */
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_NewString(ctx, "");
    JSValue result = JS_NewString(ctx, str);
    JS_FreeCString(ctx, str);
    return result;
}

void polyfill_inject_encoding(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    /* atob / btoa */
    JS_SetPropertyStr(ctx, global, "atob",
        JS_NewCFunction(ctx, js_atob, "atob", 1));
    JS_SetPropertyStr(ctx, global, "btoa",
        JS_NewCFunction(ctx, js_btoa, "btoa", 1));

    /* TextEncoder */
    JSValue textEncoder = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, textEncoder, "encode",
        JS_NewCFunction(ctx, js_text_encoder_encode, "encode", 1));
    JS_SetPropertyStr(ctx, global, "TextEncoder", textEncoder);

    /* TextDecoder */
    JSValue textDecoder = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, textDecoder, "decode",
        JS_NewCFunction(ctx, js_text_decoder_decode, "decode", 1));
    JS_SetPropertyStr(ctx, global, "TextDecoder", textDecoder);

    JS_FreeValue(ctx, global);
}
