import struct
import argparse
from merkle_builder import FirmwarePackage

def pack_firmware(firmware_path: str, output_path: str, version: int):
    """
    Assembles the firmware, Merkle root, and all block proofs into a single binary OTA package file.
    Format:
    [Header]
      Magic: "OTA\x00" (4B)
      Version: uint32_t (4B)
      Num Blocks: uint16_t (2B)
      Root Hash: 32B
      Proof Depth: uint8_t (1B)
    [Blocks (repeated Num Blocks times)]
      Block ID: uint16_t (2B)
      Block Data: 4096B
      Proof: depth * 32B
    """
    pkg = FirmwarePackage(firmware_path, version)
    pkg.summary()

    with open(output_path, "wb") as f:
        # Write Header
        # Magic "OTA\x00", version(4B), num_blocks(2B)
        f.write(struct.pack("<4sIH", b"OTA\x00", version, pkg.num_blocks))
        # Root hash(32B)
        f.write(pkg.root)
        # Proof depth(1B)
        f.write(struct.pack("<B", pkg.depth))

        # Write Blocks
        for block_id in range(pkg.num_blocks):
            block_data = pkg.blocks[block_id]
            proof = pkg.proof_bytes(block_id)
            
            f.write(struct.pack("<H", block_id))
            f.write(block_data)
            f.write(proof)

    print(f"[+] Successfully packaged {pkg.num_blocks} blocks into {output_path}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Assemble firmware into a full OTA package binary")
    parser.add_argument("firmware", help="Path to input firmware binary (.bin)")
    parser.add_argument("version", type=int, help="Firmware version integer")
    parser.add_argument("-o", "--output", default="update.ota", help="Output OTA package file path")
    args = parser.parse_args()

    pack_firmware(args.firmware, args.output, args.version)
