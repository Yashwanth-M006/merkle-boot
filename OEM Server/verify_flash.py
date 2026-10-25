import serial, struct, sys, time
from merkle_builder import FirmwarePackage
from uart_server import encode, decode, crc16

MSG_READ_REQ  = 0x20
MSG_READ_RESP = 0x21

def verify_flash(fw_path: str, version: int, port: str):
    pkg = FirmwarePackage(fw_path, version)

    ser = serial.Serial(port, 115200, timeout=10)
    time.sleep(1)

    mismatches = []

    for bid in range(pkg.num_blocks):
        # Request block
        payload = struct.pack("<H", bid)
        ser.write(encode(MSG_READ_REQ, payload))

        mt, resp = decode(ser)
        assert mt == MSG_READ_RESP, f"Unexpected {mt:#x}"
        assert len(resp) == 4096, f"Short read block {bid}"

        if resp != pkg.blocks[bid]:
            mismatches.append(bid)
            print(f"  Block {bid:03d} FAIL MISMATCH")
        else:
            print(f"  Block {bid:03d} OK")

    ser.close()

    if not mismatches:
        print(f"\nOK All {pkg.num_blocks} blocks verified - flash matches firmware")
    else:
        print(f"\nFAIL {len(mismatches)} blocks mismatched: {mismatches}")
    return len(mismatches) == 0

if __name__ == "__main__":
    verify_flash(sys.argv[1], int(sys.argv[2]), sys.argv[3])
