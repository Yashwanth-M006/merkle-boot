import serial, struct, time, time, sys
from merkle_builder import FirmwarePackage

# ── Frame format ──────────────────────────────────────────────
# [0xAA][type(1)][len(2 LE)][payload][crc16(2 LE)]
# ─────────────────────────────────────────────────────────────
MSG_OTA_START    = 0x01
MSG_BLOCK        = 0x02
MSG_OTA_END      = 0x03
MSG_ACK          = 0x10
MSG_NACK         = 0x11
MSG_OTA_COMPLETE = 0x12
MSG_BLOCK_REQ    = 0x13   # MCU → Server: "send me these blocks"
MSG_READY        = 0x14   # MCU → Server: "I am alive, send OTA_START"

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF

def encode(msg_type: int, payload: bytes) -> bytes:
    hdr   = struct.pack("<BBH", 0xAA, msg_type, len(payload))
    body  = hdr + payload
    return body + struct.pack("<H", crc16(body))

def decode(ser: serial.Serial, timeout=None) -> tuple:
    """Block until a valid frame. Returns (msg_type, payload)."""
    old_to = ser.timeout
    if timeout is not None:
        ser.timeout = timeout
    try:
        while True:
            b = ser.read(1)
            if not b:
                if timeout is not None: return None, None
                continue
            if b[0] != 0xAA:
                continue
            hdr = ser.read(3)
            if len(hdr) < 3:
                continue
            msg_type = hdr[0]
            length   = struct.unpack("<H", hdr[1:])[0]
            payload  = ser.read(length)
            crc_b    = ser.read(2)
            if len(payload) < length or len(crc_b) < 2:
                continue
            recv_crc = struct.unpack("<H", crc_b)[0]
            frame    = bytes([0xAA]) + hdr + payload
            if crc16(frame) == recv_crc:
                return msg_type, payload
    finally:
        ser.timeout = old_to

def send_ota(fw_path: str, version: int,
             port: str, corrupt: list = None):
    """
    corrupt: list of block IDs to intentionally corrupt
             (for fault injection demo)
    """
    pkg = FirmwarePackage(fw_path, version)
    pkg.summary()
    corrupt = set(corrupt or [])

    ser = serial.Serial(port, 115200, timeout=10)
    time.sleep(2)   # STM32 resets when pyserial opens port

    # ── Wait for MCU to announce its state ───────────────────────────
    # MCU sends MSG_READY  → wants a full OTA_START (no metadata or fresh)
    # MCU sends MSG_BLOCK_REQ → interrupted OTA, scan found missing blocks
    # Timeout 30s: enough for SHAKE-256 scan of 352KB even at -O0 (~10-15s)
    print("Waiting for MCU...")
    mt, payload = decode(ser, timeout=30.0)
    assert mt is not None, "MCU did not respond within 30s"

    if mt == MSG_READY:
        # Normal / fresh OTA
        start_pl = (struct.pack("<IH", version, pkg.num_blocks)
                    + pkg.root
                    + struct.pack("<B", pkg.depth))
        ser.write(encode(MSG_OTA_START, start_pl))
        # The MCU broadcasts MSG_READY every 1s. Skip any that arrive while
        # it is processing OTA_START; wait for a definitive ACK or NACK.
        while True:
            ack_mt, _ = decode(ser)
            if ack_mt == MSG_READY:
                continue   # MCU timer fired before it processed OTA_START
            break
        assert ack_mt == MSG_ACK, f"MCU rejected OTA_START (got {ack_mt:#x})"
        print(f"MCU ready — streaming {pkg.num_blocks} blocks\n")
        queue = list(range(pkg.num_blocks))

    elif mt == MSG_BLOCK_REQ:
        # Boot-time recovery: MCU scanned flash, found missing blocks
        nfail = struct.unpack("<H", payload[0:2])[0]
        missing_bids = [struct.unpack("<H", payload[2+i*2: 4+i*2])[0] for i in range(nfail)]
        print(f"[RECOVERY] MCU requested {nfail} missing blocks: {missing_bids}")
        queue = missing_bids
        # MCU is ALREADY waiting for MSG_BLOCK — do NOT send OTA_START

    else:
        raise AssertionError(f"Unexpected MCU message at startup: {mt:#x}")

    stats = {"sent": 0, "nack": 0, "recovered": 0}
    t_ota_start = time.time()
    
    while queue:
        bid = queue.pop(0)
        blk = bytearray(pkg.blocks[bid])

        if bid in corrupt:
            blk[512] ^= 0xFF          # flip one byte
            print(f"  [INJECT] block {bid:03d} corrupted")
            corrupt.discard(bid)      # ensure retransmit is clean

        proof = pkg.proof_bytes(bid)
        payload = struct.pack("<H", bid) + bytes(blk) + proof
        
        t_block_start = time.time()
        ser.write(encode(MSG_BLOCK, payload))
        stats["sent"] += 1

        while True:
            mt, resp = decode(ser)
            t_block_end = time.time()
            if mt == MSG_BLOCK_REQ:
                print("  [DEBUG] Ignored redundant MSG_BLOCK_REQ")
                continue
            
            rtt = (t_block_end - t_block_start) * 1000
            
            if mt == MSG_ACK:
                print(f"  Block {bid:03d} ✓ (RTT: {rtt:.1f} ms)")
                break
            elif mt == MSG_NACK:
                stats["nack"] += 1
                print(f"  Block {bid:03d} ✗  NACK — requeueing (RTT: {rtt:.1f} ms)")
                queue.insert(0, bid)       # push back to front, will resend clean
                break
            else:
                print(f"  Unexpected msg {mt:#x}")
                break

    # ── OTA_END ───────────────────────────────────────────────
    ser.write(encode(MSG_OTA_END, b''))
    mt, _ = decode(ser)
    t_ota_end = time.time()
    if mt == MSG_OTA_COMPLETE:
        total_time = t_ota_end - t_ota_start
        print(f"\n✓ OTA complete")
        print(f"  Blocks sent : {stats['sent']}")
        print(f"  NACKs       : {stats['nack']}")
        print(f"  Total time  : {total_time:.2f} s")
        print(f"  Throughput  : {((pkg.num_blocks * 4096) / total_time / 1024):.2f} KB/s")
        print(f"  Recovery BW : {stats['nack']}/{pkg.num_blocks} "
              f"= {stats['nack']/pkg.num_blocks*100:.1f}% (vs 100% full re-dl)")
    else:
        print(f"Unexpected final msg {mt:#x}")

    # ── Drain MCU printf output (benchmarks, jump messages) ──────────────
    # The MCU runs TABLE 2 + SHA-256 ablation benchmarks AFTER OTA_COMPLETE.
    # Key timing constraints:
    #   - Full SHAKE-256 scan (128 blocks) takes ~1951ms with no output
    #   - SHA-256 ablation (128 blocks) takes ~500ms with no output
    # So per-read timeout must be > 2s to survive those silent gaps.
    print("\n── MCU serial output ─────────────────────────────────────────")
    ser.reset_input_buffer()   # flush any stale bytes from the decode() scanner
    ser.timeout = 4.0          # must be > full-scan time (~2s) + margin
    idle_count  = 0
    line_buf    = b""
    while idle_count < 5:     # 5 consecutive 4s reads with no data = 20s max wait
        chunk = ser.read(256)
        if chunk:
            idle_count = 0
            line_buf += chunk
            while b"\n" in line_buf:
                line, line_buf = line_buf.split(b"\n", 1)
                print(line.decode("utf-8", errors="replace").rstrip("\r"))
        else:
            idle_count += 1
    if line_buf:
        print(line_buf.decode("utf-8", errors="replace").rstrip("\r"))
    print("── end of MCU output ─────────────────────────────────────────\n")

    ser.close()


if __name__ == "__main__":
    # Usage:
    #   Normal:   python uart_server.py firmware.bin 1 /dev/ttyACM0
    #   Fault:    python uart_server.py firmware.bin 1 /dev/ttyACM0 20 64 100
    corrupt_blocks = [int(x) for x in sys.argv[4:]]
    send_ota(sys.argv[1], int(sys.argv[2]), sys.argv[3],
             corrupt=corrupt_blocks)