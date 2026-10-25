# test_loopback.py
# Generates a 10-block dummy firmware, sends over UART, checks all ACKs

import os, sys
from merkle_builder import FirmwarePackage
from uart_server import send_ota

def main():
    if len(sys.argv) < 2:
        print("Usage: python test_loopback.py <PORT>")
        sys.exit(1)
        
    port = sys.argv[1]

    print("[*] Generating 10-block dummy firmware (40KB)...")
    dummy = os.urandom(10 * 4096)
    fw_path = "dummy_fw.bin"
    with open(fw_path, "wb") as f:
        f.write(dummy)

    print(f"[*] Starting OTA send loopback test on {port}...")
    # Send — no corruption
    send_ota(fw_path, version=1, port=port)

if __name__ == "__main__":
    main()
