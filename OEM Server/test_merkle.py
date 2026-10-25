import os
from merkle_builder import FirmwarePackage, verify_proof, BLOCK_SIZE, leaf_hash, get_proof

def test_phase1():
    print("--- Phase 1: Merkle Tree Verification ---")
    
    # 1. Create a dummy 512KB binary file (128 blocks of 4096 bytes)
    dummy_fw_path = "dummy.bin"
    dummy_data = os.urandom(128 * BLOCK_SIZE)
    with open(dummy_fw_path, "wb") as f:
        f.write(dummy_data)
        
    version = 1
    pkg = FirmwarePackage(dummy_fw_path, version)
    pkg.summary()
    
    # 2. Verify all 128 blocks round-trip correctly
    print(f"\nVerifying proofs for all {pkg.num_blocks} blocks...")
    all_valid = True
    for idx in range(pkg.num_blocks):
        proof = get_proof(pkg.tree, idx)
        lh = pkg.leaves[idx]
        is_valid = verify_proof(lh, idx, proof, pkg.root)
        if not is_valid:
            print(f"[-] Proof failed for valid block {idx}")
            all_valid = False
            
    if all_valid:
        print("[+] All 128 original proofs are VALID.")

    # 3. Corrupt a leaf and verify that the proof fails
    corrupt_idx = 42
    print(f"\nCorrupting block {corrupt_idx}...")
    
    # Generate corrupted data for that block
    corrupt_data = bytearray(pkg.blocks[corrupt_idx])
    corrupt_data[0] ^= 0xFF # Flip some bits
    
    corrupted_lh = leaf_hash(bytes(corrupt_data), corrupt_idx, version)
    
    # Verify the corrupted leaf against the original proof and root
    original_proof = get_proof(pkg.tree, corrupt_idx)
    is_valid_corrupt = verify_proof(corrupted_lh, corrupt_idx, original_proof, pkg.root)
    
    if not is_valid_corrupt:
        print(f"[+] Proof correctly FAILED for corrupted block {corrupt_idx}.")
    else:
        print(f"[-] Proof incorrectly SUCCEEDED for corrupted block {corrupt_idx}.")
        
    # Verify all other blocks still pass, except the corrupted one
    print(f"\nFinal Check: 128 blocks, only block {corrupt_idx} should fail.")
    pass_count = 0
    fail_count = 0
    for idx in range(pkg.num_blocks):
        proof = get_proof(pkg.tree, idx)
        
        # Use corrupted leaf hash if it's the target index, else the good one
        lh_to_test = corrupted_lh if idx == corrupt_idx else pkg.leaves[idx]
        
        if verify_proof(lh_to_test, idx, proof, pkg.root):
            pass_count += 1
        else:
            fail_count += 1
            if idx != corrupt_idx:
                print(f"[-] Unexpected failure at block {idx}")
                
    if pass_count == 127 and fail_count == 1:
        print("[+] SUCCESS! 127 blocks passed, 1 block failed as expected.")
    else:
        print(f"[-] FAILED! Passed: {pass_count}, Failed: {fail_count}")

    # Cleanup
    os.remove(dummy_fw_path)

if __name__ == "__main__":
    test_phase1()
