#ifndef MERKLE_VERIFY_H
#define MERKLE_VERIFY_H

#include <stdint.h>
#include <stdbool.h>

#define BLOCK_SIZE       4096U
#define HASH_LEN         32U
#define MAX_PROOF_DEPTH  8U    // supports up to 256 blocks

typedef struct { uint8_t h[HASH_LEN]; } hash_t;

/* Compute leaf: SHAKE-256(block || block_id(2B LE) || version(4B LE)) */
void merkle_leaf(hash_t *out,
                 const uint8_t *block,
                 uint16_t block_id,
                 uint32_t version);

/* Recompute root from leaf + proof. Returns true if matches expected_root. */
bool merkle_verify(const hash_t *leaf,
                   uint16_t block_id,
                   const hash_t *proof,
                   uint8_t depth,
                   const hash_t *expected_root);

/* Walk all blocks in flash, return list of failed block IDs */
uint16_t merkle_scan(uint32_t fw_base,
                     uint16_t num_blocks,
                     uint32_t version,
                     const hash_t *stored_leaves,
                     uint16_t *failed_out);

#endif
