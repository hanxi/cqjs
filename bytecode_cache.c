/*
 * bytecode_cache.c — global shared bytecode cache implementation
 *
 * Uses SHA-256 (via mbedTLS) to hash JS source code, and a hash table
 * with read-write lock for concurrent access. LRU eviction when full.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/md.h>

#include "bytecode_cache.h"
#include "cqjs.h"

/* ── Hash table helpers ── */

static unsigned int hash_string(const char *str, int bucket_count) {
    unsigned int h = 5381;
    while (*str) {
        h = ((h << 5) + h) + (unsigned char)*str++;
    }
    return h % (unsigned int)bucket_count;
}

/* ── Public API ── */

void bc_init(bytecode_cache_t *cache, int max_entries) {
    memset(cache, 0, sizeof(*cache));
    cache->bucket_count = max_entries * 2;  /* load factor ~0.5 */
    if (cache->bucket_count < 64) cache->bucket_count = 64;
    cache->buckets = calloc((size_t)cache->bucket_count,
                            sizeof(bytecode_entry_t *));
    cache->max_entries = max_entries;
    cache->entry_count = 0;
    pthread_rwlock_init(&cache->rwlock, NULL);
}

void bc_free(bytecode_cache_t *cache) {
    pthread_rwlock_wrlock(&cache->rwlock);
    for (int i = 0; i < cache->bucket_count; i++) {
        bytecode_entry_t *e = cache->buckets[i];
        while (e) {
            bytecode_entry_t *next = e->next;
            free(e->hash);
            free(e->bytecode);
            free(e);
            e = next;
        }
    }
    free(cache->buckets);
    cache->buckets = NULL;
    cache->entry_count = 0;
    pthread_rwlock_unlock(&cache->rwlock);
    pthread_rwlock_destroy(&cache->rwlock);
}

bytecode_entry_t *bc_lookup(bytecode_cache_t *cache, const char *hash) {
    pthread_rwlock_rdlock(&cache->rwlock);
    unsigned int idx = hash_string(hash, cache->bucket_count);
    bytecode_entry_t *e = cache->buckets[idx];
    while (e) {
        if (strcmp(e->hash, hash) == 0) {
            /* Update last_access (benign race under read lock is acceptable
             * since it's only used for approximate LRU) */
            e->last_access = cqjs_now_ms();
            pthread_rwlock_unlock(&cache->rwlock);
            return e;
        }
        e = e->next;
    }
    pthread_rwlock_unlock(&cache->rwlock);
    return NULL;
}

/* Find and remove the LRU entry (caller must hold write lock) */
static void bc_evict_lru(bytecode_cache_t *cache) {
    int64_t oldest_time = INT64_MAX;
    int oldest_bucket = -1;
    bytecode_entry_t *oldest_entry = NULL;
    bytecode_entry_t *oldest_prev = NULL;

    for (int i = 0; i < cache->bucket_count; i++) {
        bytecode_entry_t *prev = NULL;
        bytecode_entry_t *e = cache->buckets[i];
        while (e) {
            if (e->last_access < oldest_time) {
                oldest_time = e->last_access;
                oldest_bucket = i;
                oldest_entry = e;
                oldest_prev = prev;
            }
            prev = e;
            e = e->next;
        }
    }

    if (oldest_entry) {
        if (oldest_prev) {
            oldest_prev->next = oldest_entry->next;
        } else {
            cache->buckets[oldest_bucket] = oldest_entry->next;
        }
        free(oldest_entry->hash);
        free(oldest_entry->bytecode);
        free(oldest_entry);
        cache->entry_count--;
    }
}

void bc_insert(bytecode_cache_t *cache, const char *hash,
               const uint8_t *bytecode, size_t len) {
    pthread_rwlock_wrlock(&cache->rwlock);

    /* Check if already exists */
    unsigned int idx = hash_string(hash, cache->bucket_count);
    bytecode_entry_t *e = cache->buckets[idx];
    while (e) {
        if (strcmp(e->hash, hash) == 0) {
            /* Already cached, update access time */
            e->last_access = cqjs_now_ms();
            pthread_rwlock_unlock(&cache->rwlock);
            return;
        }
        e = e->next;
    }

    /* Evict if full */
    if (cache->entry_count >= cache->max_entries) {
        bc_evict_lru(cache);
    }

    /* Create new entry */
    bytecode_entry_t *entry = calloc(1, sizeof(bytecode_entry_t));
    entry->hash = strdup(hash);
    entry->bytecode = malloc(len);
    memcpy(entry->bytecode, bytecode, len);
    entry->bytecode_len = len;
    entry->last_access = cqjs_now_ms();

    /* Insert at head of bucket chain */
    entry->next = cache->buckets[idx];
    cache->buckets[idx] = entry;
    cache->entry_count++;

    pthread_rwlock_unlock(&cache->rwlock);
}

char *bc_compute_hash(const char *code, size_t code_len) {
    unsigned char digest[32];

    /* Use mbedtls_md generic API (works in mbedTLS 3.x/4.x) */
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info) {
        mbedtls_md(md_info, (const unsigned char *)code, code_len, digest);
    } else {
        /* Fallback: zero hash (should not happen) */
        memset(digest, 0, sizeof(digest));
    }

    /* Convert to hex string */
    char *hex = malloc(65);
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[64] = '\0';
    return hex;
}
