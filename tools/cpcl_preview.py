#!/usr/bin/env python3
"""Render the CG/EG bitmap blocks of a CPCL job (as produced by rastertozicox)
to PNG files, one per page, so output can be checked without printing.

Handles the vendor GZIP frame. Text/barcode commands are listed, not drawn.

Usage: python3 tools/cpcl_preview.py job.cpcl [out_prefix]
"""
import re
import struct
import sys
import zlib


def png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 1, 0, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def unframe(data):
    """Expand vendor GZIP frames (1F 8B ...) in place."""
    out, i = b"", 0
    while i < len(data):
        if data[i:i + 2] == b"\x1f\x8b" and i + 8 <= len(data):
            n = struct.unpack("<I", data[i + 4:i + 8])[0]
            frame = data[i:i + n]
            crc = struct.unpack("<I", frame[-4:])[0]
            ok = (zlib.crc32(frame[8:-4]) & 0xFFFFFFFF) == crc
            body = zlib.decompressobj(-15).decompress(frame[10:-8])
            print("gzip frame: %d -> %d bytes, frame crc %s" % (n, len(body), "OK" if ok else "BAD"))
            out += body
            i += n
        else:
            j = data.find(b"\x1f\x8b", i + 1)
            j = len(data) if j < 0 else j
            out += data[i:j]
            i = j
    return out


def main():
    src = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0]
    data = unframe(open(src, "rb").read())
    pos, page, canvas, W, H = 0, 0, None, 0, 0
    while pos < len(data):
        eol = data.find(b"\n", pos)
        if eol < 0:
            eol = len(data)
        line = data[pos:eol].rstrip(b"\r")
        m = re.match(rb"(V?CG|V?EG) (\d+) (\d+) (\d+) (\d+) ", data[pos:pos + 64])
        if m:
            bw, rows, x, y = (int(v) for v in m.groups()[1:])
            start = pos + m.end()
            if m.group(1).endswith(b"EG"):  # hex encoded
                end = start + bw * rows * 2
                blob = bytes.fromhex(data[start:end].decode())
            else:
                end = start + bw * rows
                blob = data[start:end]
            print("  %s %d x %d at (%d,%d)" % (m.group(1).decode(), bw * 8, rows, x, y))
            for r in range(rows):
                for b in range(bw):
                    v = blob[r * bw + b]
                    for k in range(8):
                        if v & (0x80 >> k):
                            px, py = x + b * 8 + k, y + r
                            if 0 <= px < W and 0 <= py < H:
                                canvas[py][px // 8] &= ~(0x80 >> (px % 8)) & 0xFF
            pos = end
            continue
        text = line.decode("gbk", "replace")
        if text.startswith("! 0"):
            parts = text.split()
            H, W = int(parts[4]), 832
            canvas = [bytearray(b"\xff" * (W // 8)) for _ in range(H)]
            print("page %d: %s" % (page + 1, text))
        elif text.startswith("PAGE-WIDTH"):
            W = int(text.split()[1])
            canvas = [bytearray(b"\xff" * ((W + 7) // 8)) for _ in range(H)]
            print("  " + text)
        elif text == "PRINT":
            page += 1
            out = "%s-p%d.png" % (prefix, page)
            png(out, W, H, canvas)
            print("  PRINT -> %s" % out)
        elif text:
            print("  " + text)
        pos = eol + 1


if __name__ == "__main__":
    main()
