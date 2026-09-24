#!/usr/bin/env python3
"""Splits a raw concatenated-MJPEG stream (back-to-back JPEG frames, each
FFD8...FFD9) into our simple length-prefixed .mjpg container:
  [4 bytes little-endian frame length][frame bytes] repeated.
Makes on-device frame reading trivial (no marker scanning needed)."""
import sys
import struct

def split_frames(data):
    frames = []
    pos = 0
    n = len(data)
    while pos < n:
        if not (data[pos] == 0xFF and data[pos+1] == 0xD8):
            raise ValueError(f"expected SOI at {pos}, got {data[pos]:02x} {data[pos+1]:02x}")
        start = pos
        pos += 2
        end = data.find(b"\xff\xd9", pos)
        if end == -1:
            raise ValueError("no EOI found for trailing frame")
        end += 2
        frames.append(data[start:end])
        pos = end
    return frames

def main():
    if len(sys.argv) != 3:
        print("usage: frame_mjpeg.py input.mjpeg output.mjpg")
        sys.exit(1)
    data = open(sys.argv[1], "rb").read()
    frames = split_frames(data)
    with open(sys.argv[2], "wb") as out:
        for f in frames:
            out.write(struct.pack("<I", len(f)))
            out.write(f)
    total = sum(len(f) for f in frames)
    print(f"{sys.argv[2]}: {len(frames)} frames, {total} bytes payload + {4*len(frames)} bytes headers = {total + 4*len(frames)} bytes")

if __name__ == "__main__":
    main()
