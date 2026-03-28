/*
 * cqjs - crypto polyfill
 *
 * Implements crypto.md5, crypto.randomBytes, crypto.aesEncrypt, crypto.rsaEncrypt
 * using mbedTLS 4.x (PSA Crypto API + legacy md.h/pk.h).
 *
 * NOTE: mbedTLS 4.x removed public entropy.h/ctr_drbg.h.
 * We use psa_generate_random() for all RNG needs.
 */

#include "polyfill.h"
#include "../cqjs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* mbedTLS headers (public API only) */
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#include <mbedtls/build_info.h>
#include <mbedtls/psa_util.h>

/* PSA Crypto for AES + RNG */
#include <psa/crypto.h>

/* Forward declaration from encoding.c */
extern char *hex_encode(const uint8_t *data, size_t len, size_t *out_len);

/* ── Helper: get bytes from JS value (ArrayBuffer or string) ── */
static uint8_t *get_bytes_from_value(JSContext *ctx, JSValueConst val,
                                      size_t *out_len) {
    /* Try ArrayBuffer first */
    size_t ab_len = 0;
    uint8_t *ab = JS_GetArrayBuffer(ctx, &ab_len, val);
    if (ab) {
        uint8_t *copy = malloc(ab_len);
        if (copy) memcpy(copy, ab, ab_len);
        *out_len = ab_len;
        return copy;
    }

    /* Fall back to string */
    const char *str = JS_ToCString(ctx, val);
    if (!str) {
        *out_len = 0;
        return NULL;
    }
    size_t slen = strlen(str);
    uint8_t *copy = malloc(slen);
    if (copy) memcpy(copy, str, slen);
    JS_FreeCString(ctx, str);
    *out_len = slen;
    return copy;
}

/* Track PSA init state */
static int g_psa_initialized = 0;

static void ensure_psa_init(void) {
    if (!g_psa_initialized) {
        psa_status_t status = psa_crypto_init();
        if (status == PSA_SUCCESS)
            g_psa_initialized = 1;
    }
}

/* ── crypto.md5(data) → hex string ── */
static JSValue js_crypto_md5(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.md5 requires 1 argument");

    size_t data_len = 0;
    uint8_t *data = get_bytes_from_value(ctx, argv[0], &data_len);
    if (!data && data_len == 0) {
        data = (uint8_t *)calloc(1, 1);
        data_len = 0;
    }

    uint8_t hash[16];

    /* Use mbedtls_md generic API (works in mbedTLS 3.x) */
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!md_info) {
        free(data);
        return JS_ThrowTypeError(ctx, "crypto.md5: MD5 not available");
    }

    int ret = mbedtls_md(md_info, data, data_len, hash);
    free(data);

    if (ret != 0)
        return JS_ThrowTypeError(ctx, "crypto.md5: hash failed");

    size_t hex_len = 0;
    char *hex_str = hex_encode(hash, 16, &hex_len);
    JSValue result = JS_NewString(ctx, hex_str ? hex_str : "");
    free(hex_str);
    return result;
}

/* ── crypto.randomBytes(size) → ArrayBuffer ── */
static JSValue js_crypto_random_bytes(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.randomBytes requires 1 argument");

    ensure_psa_init();

    int32_t size = 0;
    JS_ToInt32(ctx, &size, argv[0]);
    if (size <= 0)
        return JS_NewArrayBufferCopy(ctx, NULL, 0);

    uint8_t *buf = malloc((size_t)size);
    if (!buf)
        return JS_ThrowTypeError(ctx, "crypto.randomBytes: allocation failed");

    /* Use PSA random generator (no entropy/ctr_drbg needed) */
    psa_status_t status = psa_generate_random(buf, (size_t)size);
    if (status != PSA_SUCCESS) {
        free(buf);
        return JS_ThrowTypeError(ctx, "crypto.randomBytes: psa_generate_random failed");
    }

    JSValue result = JS_NewArrayBufferCopy(ctx, buf, (size_t)size);
    free(buf);
    return result;
}

/* ── crypto.aesEncrypt(buffer, mode, key, iv) → ArrayBuffer ── */
/* Uses PSA Crypto API for AES (mbedTLS 3.x) */
static JSValue js_crypto_aes_encrypt(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt requires 4 arguments: buffer, mode, key, iv");

    ensure_psa_init();

    /* Get plaintext */
    size_t plain_len = 0;
    uint8_t *plaintext = get_bytes_from_value(ctx, argv[0], &plain_len);
    if (!plaintext) {
        plaintext = (uint8_t *)calloc(1, 1);
        plain_len = 0;
    }

    /* Get mode string */
    const char *mode_str = JS_ToCString(ctx, argv[1]);
    if (!mode_str) {
        free(plaintext);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: invalid mode");
    }

    char mode_lower[16] = {0};
    for (int i = 0; mode_str[i] && i < 15; i++)
        mode_lower[i] = (char)tolower((unsigned char)mode_str[i]);
    JS_FreeCString(ctx, mode_str);

    /* Get key */
    size_t key_len = 0;
    uint8_t *key = get_bytes_from_value(ctx, argv[2], &key_len);
    if (!key) {
        free(plaintext);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: invalid key");
    }

    /* Get IV */
    size_t iv_len = 0;
    uint8_t *iv = get_bytes_from_value(ctx, argv[3], &iv_len);
    if (!iv) {
        iv = (uint8_t *)calloc(16, 1);
        iv_len = 0;
    }

    /* PKCS7 padding */
    int block_size = 16;
    int padding = block_size - (int)(plain_len % block_size);
    size_t padded_len = plain_len + (size_t)padding;
    uint8_t *padded = malloc(padded_len);
    if (!padded) {
        free(plaintext); free(key); free(iv);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: allocation failed");
    }
    memcpy(padded, plaintext, plain_len);
    memset(padded + plain_len, (uint8_t)padding, (size_t)padding);
    free(plaintext);

    /* Determine PSA algorithm */
    psa_algorithm_t alg;
    int need_iv = 0;
    if (strcmp(mode_lower, "cbc") == 0) {
        alg = PSA_ALG_CBC_NO_PADDING;  /* We already did PKCS7 padding manually */
        need_iv = 1;
    } else if (strcmp(mode_lower, "ecb") == 0) {
        alg = PSA_ALG_ECB_NO_PADDING;
        need_iv = 0;
    } else {
        free(padded); free(key); free(iv);
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "crypto.aesEncrypt: unsupported mode: %s", mode_lower);
        return JS_ThrowTypeError(ctx, "%s", errmsg);
    }

    if (need_iv && iv_len < 16) {
        free(padded); free(key); free(iv);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: IV too short for CBC mode");
    }

    /* Import key into PSA */
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, alg);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, (psa_key_bits_t)(key_len * 8));

    psa_key_id_t key_id;
    psa_status_t status = psa_import_key(&attributes, key, key_len, &key_id);
    free(key);

    if (status != PSA_SUCCESS) {
        free(padded); free(iv);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: invalid key");
    }

    /* Encrypt using PSA cipher */
    uint8_t *ciphertext = malloc(padded_len + 16); /* extra space for output */
    if (!ciphertext) {
        free(padded); free(iv);
        psa_destroy_key(key_id);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: allocation failed");
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    status = psa_cipher_encrypt_setup(&operation, key_id, alg);
    if (status != PSA_SUCCESS) {
        free(padded); free(iv); free(ciphertext);
        psa_destroy_key(key_id);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: setup failed");
    }

    if (need_iv) {
        status = psa_cipher_set_iv(&operation, iv, 16);
        if (status != PSA_SUCCESS) {
            free(padded); free(iv); free(ciphertext);
            psa_cipher_abort(&operation);
            psa_destroy_key(key_id);
            return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: set IV failed");
        }
    }

    size_t output_len = 0;
    size_t total_output = 0;
    status = psa_cipher_update(&operation, padded, padded_len,
                                ciphertext, padded_len + 16, &output_len);
    total_output += output_len;

    if (status != PSA_SUCCESS) {
        free(padded); free(iv); free(ciphertext);
        psa_cipher_abort(&operation);
        psa_destroy_key(key_id);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: encrypt failed");
    }

    size_t finish_len = 0;
    status = psa_cipher_finish(&operation, ciphertext + total_output,
                                padded_len + 16 - total_output, &finish_len);
    total_output += finish_len;

    psa_destroy_key(key_id);
    free(padded);
    free(iv);

    if (status != PSA_SUCCESS) {
        free(ciphertext);
        return JS_ThrowTypeError(ctx, "crypto.aesEncrypt: finish failed");
    }

    JSValue result = JS_NewArrayBufferCopy(ctx, ciphertext, total_output);
    free(ciphertext);
    return result;
}

/* ── crypto.rsaEncrypt(buffer, publicKeyPEM) → ArrayBuffer ── */
/* Uses mbedtls_pk high-level API (works in mbedTLS 3.x) */
static JSValue js_crypto_rsa_encrypt(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.rsaEncrypt requires 2 arguments: buffer, publicKeyPEM");

    /* Get plaintext */
    size_t plain_len = 0;
    uint8_t *plaintext = get_bytes_from_value(ctx, argv[0], &plain_len);
    if (!plaintext) {
        plaintext = (uint8_t *)calloc(1, 1);
        plain_len = 0;
    }

    /* Get PEM string */
    const char *pem_str = JS_ToCString(ctx, argv[1]);
    if (!pem_str) {
        free(plaintext);
        return JS_ThrowTypeError(ctx, "crypto.rsaEncrypt: invalid PEM");
    }

    /* Parse public key */
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_parse_public_key(&pk,
                                           (const unsigned char *)pem_str,
                                           strlen(pem_str) + 1);
    JS_FreeCString(ctx, pem_str);

    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        free(plaintext);
        mbedtls_pk_free(&pk);
        char msg[256];
        snprintf(msg, sizeof(msg), "crypto.rsaEncrypt: parse key failed: %s", errbuf);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    /* Import pk into PSA for encryption (mbedTLS 4.x pure PSA path) */
    ensure_psa_init();

    /* Get PSA attributes from parsed pk context */
    psa_key_attributes_t rsa_attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&rsa_attrs, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&rsa_attrs, PSA_ALG_RSA_PKCS1V15_CRYPT);

    ret = mbedtls_pk_get_psa_attributes(&pk, PSA_KEY_USAGE_ENCRYPT, &rsa_attrs);
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        free(plaintext);
        mbedtls_pk_free(&pk);
        char msg[256];
        snprintf(msg, sizeof(msg), "crypto.rsaEncrypt: get PSA attributes failed: %s", errbuf);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    /* Override algorithm to RSA PKCS1 v1.5 */
    psa_set_key_algorithm(&rsa_attrs, PSA_ALG_RSA_PKCS1V15_CRYPT);

    /* Import into PSA */
    psa_key_id_t rsa_key_id;
    ret = mbedtls_pk_import_into_psa(&pk, &rsa_attrs, &rsa_key_id);
    mbedtls_pk_free(&pk);

    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        free(plaintext);
        char msg[256];
        snprintf(msg, sizeof(msg), "crypto.rsaEncrypt: PSA import failed: %s", errbuf);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    /* Get key size in bytes for output buffer */
    psa_key_attributes_t imported_attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_get_key_attributes(rsa_key_id, &imported_attrs);
    size_t key_bits = psa_get_key_bits(&imported_attrs);
    size_t key_bytes = (key_bits + 7) / 8;
    psa_reset_key_attributes(&imported_attrs);

    uint8_t *output = malloc(key_bytes);
    if (!output) {
        free(plaintext);
        psa_destroy_key(rsa_key_id);
        return JS_ThrowTypeError(ctx, "crypto.rsaEncrypt: allocation failed");
    }

    /* Encrypt using PSA */
    size_t olen = 0;
    psa_status_t psa_ret = psa_asymmetric_encrypt(
        rsa_key_id, PSA_ALG_RSA_PKCS1V15_CRYPT,
        plaintext, plain_len,
        NULL, 0,  /* no salt for PKCS1v15 */
        output, key_bytes, &olen);

    psa_destroy_key(rsa_key_id);
    free(plaintext);

    if (psa_ret != PSA_SUCCESS) {
        free(output);
        char msg[128];
        snprintf(msg, sizeof(msg), "crypto.rsaEncrypt: PSA encrypt failed (status=%d)", (int)psa_ret);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    JSValue result = JS_NewArrayBufferCopy(ctx, output, olen);
    free(output);
    return result;
}

/* ── Inject crypto object into global scope ── */
void polyfill_inject_crypto(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue crypto = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, crypto, "md5",
        JS_NewCFunction(ctx, js_crypto_md5, "md5", 1));
    JS_SetPropertyStr(ctx, crypto, "randomBytes",
        JS_NewCFunction(ctx, js_crypto_random_bytes, "randomBytes", 1));
    JS_SetPropertyStr(ctx, crypto, "aesEncrypt",
        JS_NewCFunction(ctx, js_crypto_aes_encrypt, "aesEncrypt", 4));
    JS_SetPropertyStr(ctx, crypto, "rsaEncrypt",
        JS_NewCFunction(ctx, js_crypto_rsa_encrypt, "rsaEncrypt", 2));

    JS_SetPropertyStr(ctx, global, "crypto", crypto);
    JS_FreeValue(ctx, global);
}
