#!/usr/bin/env python3
"""Generate a minimal uncompressed 16-bit CFA (RGGB) DNG for the LibRaw smoke test.

Usage:
    make_test_dng.py                    # default 64x48 gradient test.dng (smoke)
    make_test_dng.py 512 384 scene.dng  # larger scenic pattern for browser demo
"""
import struct
import sys

W, H = 64, 48
PAT = (0, 1, 1, 2)  # RGGB

NUM_TAGS = 21
HDR = 8
ENTRY_START = HDR + 2
ENTRY_END = ENTRY_START + NUM_TAGS * 12
EXTRA_START = ENTRY_END + 4  # after next-IFD pointer


def raw_of(x, y, w, h, big):
    """Return the 16-bit value for CFA channel at (x,y).

    big: scenic scene with radial gradient, sky wedge and color patches.
    Otherwise: the original linear gradient used by the smoke checks.
    """
    if not big:
        ch = PAT[(y % 2) * 2 + (x % 2)]
        if ch == 0:
            return x * 65535 // (w - 1)
        if ch == 1:
            return y * 65535 // (h - 1)
        return (x + y) * 65535 // (w + h - 2)

    # Scenic scene: expose a wide dynamic range so the tone sliders are visible.
    fx = x / max(1, w - 1)
    fy = y / max(1, h - 1)
    r = ((fx - 0.5) ** 2 + (fy - 0.5) ** 2) ** 0.5 * 1.414  # 0..~1 corner dist

    # vertical sky (high) -> ground (low) gradient
    base = 0.85 - 0.80 * fy
    # radial hotspot top-left, strong falloff elsewhere
    hs = ((fx - 0.18) ** 2 + (fy - 0.15) ** 2) ** 0.5
    base += 0.65 * max(0.0, 1.0 - hs * 3.0)
    # few saturated color patches far down the scale
    if 0.35 <= fy <= 0.55 and 0.25 <= fx <= 0.40:
        base = (0.08, 0.62, 0.70)[(int(fx * 10) % 3)]  # red / green / blue-ish
    if 0.60 <= fy <= 0.80 and 0.55 <= fx <= 0.72:
        base = 0.10 if (x + y) % 2 == 0 else 0.35       # checkerboard
    v = max(0.0, min(1.0, base)) * 65535.0
    return int(v)


def main():
    global W, H
    outfile = "test.dng"
    scenic = False
    if len(sys.argv) >= 3:
        W, H = int(sys.argv[1]), int(sys.argv[2])
        outfile = sys.argv[3]
        scenic = True
    elif len(sys.argv) == 2:
        outfile = sys.argv[1]

    pixels = bytearray()
    for y in range(H):
        for x in range(W):
            pixels += struct.pack("<H", raw_of(x, y, W, H, scenic))

    # --- Phase 1: build the "extra" blob region and record offsets ---
    extra = bytearray()
    offs = {}

    def blob(payload):
        nonlocal extra
        off = EXTRA_START + len(extra)
        extra += payload
        if len(payload) % 2:
            extra += b"\x00"
        return off

    offs["cfa_repeat"] = blob(struct.pack("<HH", 2, 2))
    offs["cfa_pattern"] = blob(bytes(PAT))
    offs["cfa_plane"] = blob(bytes((0, 1, 2)))
    offs["dng_version"] = blob(bytes((1, 4, 0, 0)))
    model = b"WasmRawTest\0"
    offs["unique_model"] = blob(model)
    offs["black"] = blob(struct.pack("<II", 0, 1))                    # BlackLevel 0/1
    offs["white"] = blob(struct.pack("<I", 65535))                    # WhiteLevel
    cm = (1,1, 0,1, 0,1, 0,1, 1,1, 0,1, 0,1, 0,1, 1,1)                # identity 3x3
    offs["cmatrix"] = blob(struct.pack("<" + "ii" * 9, *cm))          # ColorMatrix1
    offs["neutral"] = blob(struct.pack("<IIIIII", 1,1, 1,1, 1,1))     # AsShotNeutral 1,1,1

    while len(extra) % 4:
        extra += b"\x00"
    DATA_OFF = EXTRA_START + len(extra)

    # --- Phase 2: emit TIFF/DNG structure ---
    out = bytearray()
    out += b"II" + struct.pack("<H", 42) + struct.pack("<I", HDR)
    out += struct.pack("<H", NUM_TAGS)

    def add(tag, typ, count, value):
        nonlocal out
        if typ == 3 and count == 1:
            body = struct.pack("<H", value) + b"\x00\x00"
        elif typ == 4 and count == 1:
            body = struct.pack("<I", value)
        else:
            body = struct.pack("<I", value)
        out += struct.pack("<HHI", tag, typ, count) + body

    add(256, 4, 1, W)
    add(257, 4, 1, H)
    add(258, 3, 1, 16)
    add(259, 3, 1, 1)
    add(262, 3, 1, 32803)                        # CFA
    add(273, 4, 1, DATA_OFF)                      # StripOffsets
    add(277, 3, 1, 1)
    add(278, 4, 1, H)
    add(279, 4, 1, len(pixels))
    add(284, 3, 1, 1)
    add(339, 3, 1, 1)                             # SampleFormat unsigned int
    add(33421, 3, 2, offs["cfa_repeat"])          # CFAPatternRepeatDim
    add(33422, 1, 4, offs["cfa_pattern"])         # CFAPattern
    add(50706, 1, 4, offs["dng_version"])         # DNGVersion
    add(50708, 2, len(model), offs["unique_model"])  # UniqueCameraModel
    add(50710, 1, 3, offs["cfa_plane"])           # CFAPlaneColor
    add(50711, 3, 1, 1)                           # CFALayout
    add(50714, 5, 1, offs["black"])               # BlackLevel
    add(50717, 4, 1, 65535)                       # WhiteLevel
    add(50721, 10, 9, offs["cmatrix"])            # ColorMatrix1
    add(50728, 5, 3, offs["neutral"])             # AsShotNeutral

    out += struct.pack("<I", 0)                   # next IFD

    assert len(out) == EXTRA_START, (len(out), EXTRA_START)
    for k, off in offs.items():
        assert off + 1 <= DATA_OFF, (k, off)
    assert len(extra) % 4 == 0
    out += extra + pixels

    with open(outfile, "wb") as f:
        f.write(out)
    print(f"wrote {outfile}: {len(out)} bytes, data_off={DATA_OFF}, extra={len(extra)} pixels={len(pixels)}")


if __name__ == "__main__":
    main()