#!/usr/bin/env python3
"""Generate a minimal uncompressed 16-bit RGB TIFF for the LibRaw smoke test."""
import struct

W, H = 64, 48
NUM_TAGS = 11

# Layout:
#   0..8   header (II, 42, IFD offset=8)
#   8..10  tag count
#   10..142  (3 + 11*12) entry table -> ends at 10+132=142
#   142..146 next IFD = 0
#   146..   extra multi-short arrays
BITS_OFF = 146
SAMPLES_OFF = BITS_OFF + 8  # BitsPerSample: 6 bytes + 2 padding
DATA_OFF = SAMPLES_OFF + 8


def main():
    pixels = bytearray()
    for y in range(H):
        for x in range(W):
            r = (x * 65535) // (W - 1)
            g = (y * 65535) // (H - 1)
            b = ((x + y) * 65535) // (W + H - 2)
            for v in (r, g, b):
                pixels += struct.pack("<H", v)

    out = bytearray()
    out += b"II" + struct.pack("<H", 42) + struct.pack("<I", 8)
    out += struct.pack("<H", NUM_TAGS)

    entries = []
    def add(tag, typ, count, value):
        entries.append((tag, typ, count, value))

    add(256, 4, 1, W)          # ImageWidth
    add(257, 4, 1, H)          # ImageLength
    add(258, 3, 3, BITS_OFF)   # BitsPerSample (3 x short)
    add(259, 3, 1, 1)          # Compression = none
    add(262, 3, 1, 2)          # PhotometricInterpretation = RGB
    add(273, 4, 1, DATA_OFF)   # StripOffsets
    add(277, 3, 1, 3)          # SamplesPerPixel
    add(278, 4, 1, H)          # RowsPerStrip
    add(279, 4, 1, len(pixels))# StripByteCounts
    add(284, 3, 1, 1)          # PlanarConfiguration = chunky
    add(339, 3, 3, SAMPLES_OFF)# SampleFormat (3 x short, unsigned int)

    for tag, typ, count, value in entries:
        if typ == 3 and count == 1:
            val4 = struct.pack("<H", value) + b"\x00\x00"
        else:
            val4 = struct.pack("<I", value)
        out += struct.pack("<HHI", tag, typ, count) + val4

    out += struct.pack("<I", 0)         # next IFD

    # extra arrays, padded to 8 bytes each
    assert len(out) == BITS_OFF, len(out)
    out += struct.pack("<HHH", 16, 16, 16) + b"\x00\x00"
    assert len(out) == SAMPLES_OFF, len(out)
    out += struct.pack("<HHH", 16, 16, 16) + b"\x00\x00"
    assert len(out) == DATA_OFF, len(out)
    out += pixels

    with open("test.tiff", "wb") as f:
        f.write(out)
    print(f"wrote test.tiff: {len(out)} bytes, pixel data at {DATA_OFF} ({len(pixels)} bytes)")


if __name__ == "__main__":
    main()