#!/usr/bin/env python3
"""Build the raw UFS LUN0 image the kernel's UFS driver (bsd/dev/sweet_ufs.c) expects, as a SPARSE file:
4096-byte blocks, protective MBR (LBA0), GPT header (LBA1), 128 entries x 128 bytes (LBA2..5), ONE partition
named USERDATA whose first block holds the HFS+ root volume, and 4096 spare blocks at its end (the kernel's console
ring lives in the last 1025 blocks of the partition).
usage: make_gpt.py ROOT_IMAGE OUT_DISK [FIRST_LBA=256]
Only the GPT blocks and the root image's non-zero 1 MiB runs are written (zero runs stay holes)."""
import os, struct, sys, uuid, zlib

root, out = sys.argv[1], sys.argv[2]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 256
BS = 4096
rsize = os.path.getsize(root)
assert rsize % BS == 0
nblocks = rsize // BS + 4096           # partition length in blocks (root + >=1025 log blocks, rounded generously)
last = first + nblocks - 1
total = last + 1 + 6                    # + room for a backup GPT (entries + header), not read by the kernel
ent = bytearray(128 * 128)
name = 'USERDATA'.encode('utf-16le')
e = struct.pack('<16s16sQQQ', uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le,
                uuid.UUID('11111111-2222-3333-4444-555555555555').bytes_le, first, last, 0) + name
ent[0:len(e)] = e
hdr = bytearray(92)
struct.pack_into('<8sIIIIQQQQ16sQIII', hdr, 0, b'EFI PART', 0x10000, 92, 0, 0, 1, total - 1, 6,
                 last, uuid.UUID('aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee').bytes_le, 2, 128, 128,
                 zlib.crc32(bytes(ent)) & 0xffffffff)
struct.pack_into('<I', hdr, 16, zlib.crc32(bytes(hdr)) & 0xffffffff)
mbr = bytearray(BS)
mbr[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1, min(total - 1, 0xffffffff))
mbr[510:512] = b'\x55\xaa'
with open(out, 'wb') as f:
    f.truncate(total * BS)
    f.seek(0); f.write(mbr)
    f.seek(BS); f.write(hdr)
    f.seek(2 * BS); f.write(ent)
    with open(root, 'rb') as r:
        pos, step = 0, 1 << 20
        while True:
            chunk = r.read(step)
            if not chunk:
                break
            if chunk.count(0) != len(chunk):
                f.seek(first * BS + pos); f.write(chunk)
            pos += len(chunk)
print('disk %s: %d blocks of 4096 (%d MiB apparent), USERDATA first=%#x last=%#x' % (out, total, total * BS >> 20, first, last))
