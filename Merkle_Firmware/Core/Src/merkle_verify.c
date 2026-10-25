#include "merkle_verify.h"
#include "../../Lib/sha3.h"      // SHAKE-256 implementation (Markku-Juhani Saarinen)
#include <string.h>

/*
 * SHAKE-256 wrapper using the sha3.h context API:
 *   shake256_init → shake_update → shake_xof → shake_out
 */
static void do_shake256(const uint8_t *data, uint32_t len, uint8_t *out32)
{
    sha3_ctx_t ctx;
    shake256_init(&ctx);                    // init for SHAKE-256 (rsiz = 200 - 2*32 = 136)
    shake_update(&ctx, data, len);          // absorb
    shake_xof(&ctx);                        // switch to squeeze mode
    shake_out(&ctx, out32, HASH_LEN);       // squeeze 32 bytes
}

static void hash_pair(const hash_t *left, const hash_t *right, hash_t *out)
{
    uint8_t buf[HASH_LEN * 2];
    memcpy(buf,            left->h,  HASH_LEN);
    memcpy(buf + HASH_LEN, right->h, HASH_LEN);
    do_shake256(buf, sizeof(buf), out->h);
}

void merkle_leaf(hash_t *out,
                 const uint8_t *block,
                 uint16_t block_id,
                 uint32_t version)
{
    /* Allocate a temporary buffer: block + 6 bytes metadata */
    /* For 4 KB block this is stack-heavy; use static carefully */
    static uint8_t tmp[BLOCK_SIZE + 6];
    memcpy(tmp, block, BLOCK_SIZE);
    tmp[BLOCK_SIZE + 0] = block_id & 0xFF;
    tmp[BLOCK_SIZE + 1] = (block_id >> 8) & 0xFF;
    tmp[BLOCK_SIZE + 2] = version & 0xFF;
    tmp[BLOCK_SIZE + 3] = (version >> 8) & 0xFF;
    tmp[BLOCK_SIZE + 4] = (version >> 16) & 0xFF;
    tmp[BLOCK_SIZE + 5] = (version >> 24) & 0xFF;
    do_shake256(tmp, BLOCK_SIZE + 6, out->h);
}

bool merkle_verify(const hash_t *leaf,
                   uint16_t block_id,
                   const hash_t *proof,
                   uint8_t depth,
                   const hash_t *expected_root)
{
    hash_t current;
    memcpy(&current, leaf, sizeof(hash_t));

    uint16_t idx = block_id;
    for (uint8_t lvl = 0; lvl < depth; lvl++) {
        hash_t next;
        if (idx & 1)
            hash_pair(&proof[lvl], &current, &next);  // sibling left
        else
            hash_pair(&current, &proof[lvl], &next);  // sibling right
        current = next;
        idx >>= 1;
    }
    return memcmp(current.h, expected_root->h, HASH_LEN) == 0;
}
static bool block_is_erased(uint32_t addr)
{
    volatile uint32_t *p = (volatile uint32_t*)addr;
    /* Only check the first few words to quickly identify erased blocks,
       or check the whole block. 1024 words = 4096 bytes. */
    for (int i = 0; i < 1024; i++) {
        if (p[i] != 0xFFFFFFFF) return false;
    }
    return true;
}

uint16_t merkle_scan(uint32_t fw_base,
                     uint16_t num_blocks,
                     uint32_t version,
                     const hash_t *stored_leaves,
                     uint16_t *failed_out)
{
    uint16_t count = 0;
    for (uint16_t i = 0; i < num_blocks; i++) {
        uint32_t addr = fw_base + i * 4096;
        
        /* Fast path: erased block is obviously corrupt */
        if (block_is_erased(addr)) {
            failed_out[count++] = i;
            continue;
        }

        /* Slow path: hash and compare */
        hash_t live;
        merkle_leaf(&live, (uint8_t*)addr, i, version);
        if (memcmp(live.h, stored_leaves[i].h, HASH_LEN) != 0) {
            failed_out[count++] = i;
        }
    }
    return count;  // 0 = all good
}
