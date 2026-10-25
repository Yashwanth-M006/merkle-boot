"""
build.py - Build the Tier 3 test application and produce OTA-ready artifacts.

Strategy
--------
1. Compile + link the tiny LED-blink code (~500 B) to an ELF/bin.
2. In Python, pad the bin to exactly TARGET_KB kilobytes with
   block-indexed watermarks so every 4 KB block is uniquely identifiable
   during fault-injection tests.
3. Wrap the padded binary back into an Intel HEX file (CubeProgrammer can
   flash .hex files directly by address).

Artifacts produced in this directory:
  tier3_code.bin   - raw compiled code (tiny, ~500 B)
  tier3.bin        - padded to TARGET_KB, block-watermarked, use with uart_server.py
  tier3.hex        - Intel HEX at 0x08008000, flash via CubeProgrammer
  tier3.elf        - ELF of the padded binary, loadable by GDB/CubeIDE

Usage:
  python build.py
"""

import subprocess, struct, os, sys

# ── Config ────────────────────────────────────────────────────────────────────
TARGET_KB   = 300          # size of final binary
BLOCK_SIZE  = 4096         # OTA block size
FILL_BYTE   = 0xAA         # fill pattern for unoccupied areas
LOAD_ADDR   = 0x08008000   # Tier 3 base address

TOOLCHAIN = (
    r"C:\Users\Yashwanth\Downloads"
    r"\arm-gnu-toolchain-15.2.rel1-mingw-w64-i686-arm-none-eabi\bin"
)
GCC      = os.path.join(TOOLCHAIN, "arm-none-eabi-gcc.exe")
OBJCOPY  = os.path.join(TOOLCHAIN, "arm-none-eabi-objcopy.exe")
SIZE_EXE = os.path.join(TOOLCHAIN, "arm-none-eabi-size.exe")

HERE = os.path.dirname(os.path.abspath(__file__))

FLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
    "-Os", "-ffunction-sections", "-fdata-sections",
    "--specs=nano.specs", "--specs=nosys.specs",
]

def run(cmd):
    print("$", " ".join(str(c) for c in cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.stdout.strip(): print(r.stdout.rstrip())
    if r.stderr.strip(): print(r.stderr.rstrip())
    if r.returncode != 0:
        sys.exit(f"[FAIL] exit {r.returncode}")

p = lambda *parts: os.path.join(HERE, *parts)

# ── 1. Compile and link the tiny code ────────────────────────────────────────
run([GCC] + FLAGS + ["-c", p("startup.s"),     "-o", p("startup.o")])
run([GCC] + FLAGS + ["-c", p("tier3_main.c"),  "-o", p("tier3_main.o")])
run([GCC] + FLAGS + [
    p("startup.o"), p("tier3_main.o"),
    f"-T{p('tier3.ld')}",
    "-Wl,--gc-sections",
    "-Wl,-Map=" + p("tier3.map"),
    "-o", p("tier3_code.elf"),
])
run([SIZE_EXE, p("tier3_code.elf")])

# Extract code-only binary
run([OBJCOPY, "-O", "binary", p("tier3_code.elf"), p("tier3_code.bin")])

with open(p("tier3_code.bin"), "rb") as f:
    code = bytearray(f.read())

print(f"\nCompiled code size : {len(code):,} bytes")

# ── 2. Pad to TARGET_KB with block-ID watermarks ─────────────────────────────
TARGET_SIZE = TARGET_KB * 1024
if len(code) > TARGET_SIZE:
    sys.exit(f"Code ({len(code):,} B) exceeds target ({TARGET_SIZE:,} B)!")

# Start with FILL_BYTE
data = bytearray([FILL_BYTE] * TARGET_SIZE)

# Overwrite the start with our compiled code
data[:len(code)] = code

NUM_BLOCKS = TARGET_SIZE // BLOCK_SIZE
print(f"Padding to         : {TARGET_SIZE:,} bytes  ({NUM_BLOCKS} x {BLOCK_SIZE} B blocks)")

# Write block-ID watermarks at 4 positions in each block
# (skip block 0 which contains the vector table and code)
for blk in range(1, NUM_BLOCKS):
    base = blk * BLOCK_SIZE
    for off in (0, 1024, 2048, 3072):
        if base + off + 4 <= TARGET_SIZE:
            struct.pack_into("<I", data, base + off, blk)

with open(p("tier3.bin"), "wb") as f:
    f.write(data)

print(f"tier3.bin written  : {len(data):,} bytes")

# ── 3. Intel HEX (CubeProgrammer can flash this directly at 0x08008000) ──────
run([OBJCOPY,
     "-I", "binary",
     "-O", "ihex",
     f"--change-addresses={LOAD_ADDR}",
     p("tier3.bin"), p("tier3.hex")])

# ── 4. ELF wrapper (optional, for GDB/CubeIDE load) ──────────────────────────
run([OBJCOPY,
     "-I", "binary",
     "-O", "elf32-littlearm",
     "--binary-architecture", "arm",
     f"--change-addresses={LOAD_ADDR}",
     p("tier3.bin"), p("tier3.elf")])

# ── Summary ───────────────────────────────────────────────────────────────────
print("\nArtifacts:")
for fn in ("tier3_code.bin", "tier3.bin", "tier3.hex", "tier3.elf"):
    pp = p(fn)
    print(f"  {fn:16s}  {os.path.getsize(pp):>10,} bytes   {pp}")

print("""
To flash via STM32CubeProgrammer (Erasing & Programming panel):
  1. Browse  ->  tier3.bin
  2. Start Address:  0x08008000
  3. Click [Start Programming]

To test OTA:
  cd ..\\OEM Server
  python uart_server.py ..\\Tier3App\\tier3.bin 1 COM5

To test fault injection (corrupt blocks 5, 20, 50):
  python uart_server.py ..\\Tier3App\\tier3.bin 1 COM5 5 20 50
""")
