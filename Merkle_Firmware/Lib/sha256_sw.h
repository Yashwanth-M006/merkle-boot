/*
 * sha256_sw.h  — Minimal portable SHA-256 (FIPS 180-4)
 *
 * Used ONLY for ablation benchmarking against SHAKE-256.
 * NOT used in the production OTA path.
 *
 * Public domain / unlicense.
 */
#ifndef SHA256_SW_H
#define SHA256_SW_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} sha256_ctx_t;

void sha256_init   (sha256_ctx_t *ctx);
void sha256_update (sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final  (sha256_ctx_t *ctx, uint8_t digest[32]);

/* One-shot convenience */
void sha256_oneshot(const uint8_t *data, size_t len, uint8_t digest[32]);

#endif /* SHA256_SW_H */
