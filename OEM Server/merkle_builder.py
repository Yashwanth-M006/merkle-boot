import hashlib
import math
import struct
from pathlib import Path

BLOCK_SIZE = 4096   # 4 KB — one Merkle leaf
HASH_LEN   = 32     # SHAKE-256 output

def shake256(data: bytes) -> bytes:
    return hashlib.shake_256(data).digest(HASH_LEN)

def leaf_hash(block_data: bytes, block_id: int, version: int) -> bytes:
    """
    SHAKE-256(block_data || block_id(2B LE) || version(4B LE))
    Binding block_id prevents block-swap attacks.
    Binding version prevents cross-version proof reuse.
    """
    meta = struct.pack("<HI", block_id, version)
    return shake256(block_data + meta)

def parent_hash(left: bytes, right: bytes) -> bytes:
    return shake256(left + right)

def pad_leaves(leaves: list) -> list:
    """Pad to next power of two using last leaf (deterministic)."""
    n = len(leaves)
    if n == 0:
        raise ValueError("No leaves")
    target = 1 << math.ceil(math.log2(max(n, 2)))
    while len(leaves) < target:
        leaves.append(leaves[-1])  # repeat last leaf
    return leaves

def build_tree(leaves: list) -> list:
    """
    Returns full tree as list of levels.
    tree[0] = leaf level (padded)
    tree[-1] = [root]
    """
    padded = pad_leaves(leaves[:])
    levels = [padded]
    current = padded
    while len(current) > 1:
        nxt = []
        for i in range(0, len(current), 2):
            nxt.append(parent_hash(current[i], current[i+1]))
        levels.append(nxt)
        current = nxt
    return levels

def get_root(tree: list) -> bytes:
    return tree[-1][0]

def get_proof(tree: list, idx: int) -> list:
    """
    Sibling hashes from leaf to root.
    len = tree depth = log2(num_padded_leaves)
    """
    proof = []
    for level in tree[:-1]:
        sibling = idx ^ 1
        proof.append(level[sibling])
        idx //= 2
    return proof

def verify_proof(leaf: bytes, block_id: int,
                 proof: list, root: bytes) -> bool:
    """Recompute root from leaf + proof. Compare with expected root."""
    current = leaf
    idx = block_id
    for sibling in proof:
        if idx & 1:
            current = parent_hash(sibling, current)
        else:
            current = parent_hash(current, sibling)
        idx //= 2
    return current == root

class FirmwarePackage:
    def __init__(self, path: str, version: int):
        with open(path, 'rb') as f:
            data = f.read()

        # Pad firmware to block boundary
        rem = len(data) % BLOCK_SIZE
        if rem:
            data += b'\xff' * (BLOCK_SIZE - rem)

        self.data      = data
        self.version   = version
        self.blocks    = [data[i:i+BLOCK_SIZE]
                          for i in range(0, len(data), BLOCK_SIZE)]
        self.num_blocks = len(self.blocks)

        self.leaves = [leaf_hash(b, i, version)
                       for i, b in enumerate(self.blocks)]
        self.tree   = build_tree(self.leaves[:])
        self.root   = get_root(self.tree)
        self.depth  = len(self.tree) - 1

    def proof_bytes(self, block_id: int) -> bytes:
        return b''.join(get_proof(self.tree, block_id))

    def summary(self):
        print(f"Firmware : {len(self.data)} bytes")
        print(f"Blocks   : {self.num_blocks}")
        print(f"Depth    : {self.depth}")
        print(f"Root     : {self.root.hex()}")