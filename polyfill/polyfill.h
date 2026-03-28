#ifndef POLYFILL_H
#define POLYFILL_H

#include "../quickjs/quickjs.h"
#include "../cqjs.h"

/* Managers returned by polyfill_inject_all */
typedef struct {
    timer_manager_t *timer_mgr;
    fetch_manager_t *fetch_mgr;
} polyfill_managers_t;

/* Inject all polyfills, returns managers for event loop */
polyfill_managers_t *polyfill_inject_all(JSContext *ctx);

/* Free all managers */
void polyfill_close(polyfill_managers_t *pm, JSContext *ctx);

/* Individual polyfill inject functions */
void polyfill_inject_console(JSContext *ctx);
void polyfill_inject_encoding(JSContext *ctx);
void polyfill_inject_buffer(JSContext *ctx);
void polyfill_inject_url(JSContext *ctx);
void polyfill_inject_timer(JSContext *ctx, timer_manager_t *tm);
void polyfill_inject_fetch(JSContext *ctx, fetch_manager_t *fm);
void polyfill_inject_crypto(JSContext *ctx);
void polyfill_inject_zlib(JSContext *ctx);

/* Base64/hex helpers (used by encoding.c and buffer.c) */
char *base64_encode(const uint8_t *data, size_t len, size_t *out_len);
uint8_t *base64_decode(const char *src, size_t src_len, size_t *out_len);
char *hex_encode(const uint8_t *data, size_t len, size_t *out_len);
uint8_t *hex_decode(const char *src, size_t src_len, size_t *out_len);

#endif /* POLYFILL_H */
