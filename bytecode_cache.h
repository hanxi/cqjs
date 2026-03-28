#ifndef BYTECODE_CACHE_H
#define BYTECODE_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* ============================================================
 * bytecode_cache — global shared bytecode cache
 *
 * Caches compiled JS bytecode (serialized via JS_WriteObject)
 * keyed by SHA-256 hash of the source code.
 * Thread-safe via pthread_rwlock (read-heavy workload).
 * ============================================================ */

typedef struct bytecode_entry {
    char *hash;             /* SHA-256 hex string (64 chars + NUL) */
    uint8_t *bytecode;      /* JS_WriteObject serialized bytecode */
    size_t bytecode_len;
    int64_t last_access;    /* monotonic timestamp for LRU eviction */
    struct bytecode_entry *next;  /* hash bucket chain */
} bytecode_entry_t;

typedef struct {
    bytecode_entry_t **buckets;
    int bucket_count;
    int entry_count;
    int max_entries;
    pthread_rwlock_t rwlock;  /* read-write lock: reads are concurrent */
} bytecode_cache_t;

/* Initialize cache with given max entry count */
void bc_init(bytecode_cache_t *cache, int max_entries);

/* Free all cache entries and resources */
void bc_free(bytecode_cache_t *cache);

/* Lookup bytecode by hash. Returns entry (caller must NOT free) or NULL.
 * Caller must hold at least a read lock (handled internally). */
bytecode_entry_t *bc_lookup(bytecode_cache_t *cache, const char *hash);

/* Insert bytecode into cache. Makes copies of hash and bytecode data.
 * Evicts LRU entry if cache is full. */
void bc_insert(bytecode_cache_t *cache, const char *hash,
               const uint8_t *bytecode, size_t len);

/* Compute SHA-256 hash of code. Returns malloc'd hex string (64 chars).
 * Caller must free. */
char *bc_compute_hash(const char *code, size_t code_len);

#endif /* BYTECODE_CACHE_H */
