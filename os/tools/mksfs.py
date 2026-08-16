#!/usr/bin/env python3
"""Build a minimal SFS image containing embedded ELF binaries."""

import struct
import sys
import os

SFS_MAGIC      = 0x53465301
SFS_BLOCK_SIZE = 512
SFS_MAX_INODES = 1024
SFS_DIRECT     = 12
SFS_TYPE_FILE  = 1
SFS_TYPE_DIR   = 2

def u32_le(v): return struct.pack('<I', v)
def u16_le(v): return struct.pack('<H', v)

def blocks_for_bmap(bits):
    return (bits + SFS_BLOCK_SIZE * 8 - 1) // (SFS_BLOCK_SIZE * 8)

def build_sfs(files, out_path, total_blocks):
    journal_blocks = 256  # match kernel JOURNAL_BLOCKS (journal.h)
    usable = total_blocks - journal_blocks
    if usable < 16:
        print("too few blocks", file=sys.stderr); sys.exit(1)

    ibs = blocks_for_bmap(SFS_MAX_INODES)
    bbs = blocks_for_bmap(usable)
    its = (SFS_MAX_INODES * 88 + SFS_BLOCK_SIZE - 1) // SFS_BLOCK_SIZE
    ds = 1 + ibs + bbs + its  # data_start

    import io
    image = io.BytesIO()
    image.write(b'\x00' * total_blocks * SFS_BLOCK_SIZE)

    # superblock
    sb = bytearray(512)
    struct.pack_into('<I', sb, 0, SFS_MAGIC)
    struct.pack_into('<I', sb, 4, SFS_MAX_INODES)
    struct.pack_into('<I', sb, 8, usable)
    struct.pack_into('<I', sb, 12, 1)            # inode_bmap_start
    struct.pack_into('<I', sb, 16, ibs + 1)       # block_bmap_start
    struct.pack_into('<I', sb, 20, ibs + bbs + 1) # inode_table_start
    struct.pack_into('<I', sb, 24, ds)            # data_start
    struct.pack_into('<I', sb, 28, 0)             # root_inode
    struct.pack_into('<I', sb, 32, 0)             # checksum placeholder

    ck = 0
    for i in range(0, 512, 4):
        if i == 32: continue
        ck ^= struct.unpack_from('<I', sb, i)[0]
    struct.pack_into('<I', sb, 32, ck)  # XOR checksum

    image.seek(0); image.write(sb)

    def set_bit(buf, bit):
        buf[bit >> 3] |= 1 << (bit & 7)

    # inode bitmap: mark inode 0 (root) as used, then allocated for each file
    inode_bmap = bytearray(ibs * SFS_BLOCK_SIZE)
    set_bit(inode_bmap, 0)
    inode_num = 1
    all_files = list(files.items())
    finos = {}
    for name, data in all_files:
        finos[name] = inode_num
        set_bit(inode_bmap, inode_num)
        inode_num += 1
    image.seek(1 * SFS_BLOCK_SIZE); image.write(inode_bmap)

    # block bitmap: mark meta blocks as used, then data blocks as files are written
    block_bmap = bytearray(bbs * SFS_BLOCK_SIZE)
    for b in range(ds):
        set_bit(block_bmap, b)

    # inode table
    itbl = bytearray(its * SFS_BLOCK_SIZE)
    def write_inode(inum, typ, size, direct, nlink=1):
        off = inum * 88
        struct.pack_into('<H', itbl, off, typ)
        struct.pack_into('<H', itbl, off+2, 0o644 if typ == SFS_TYPE_FILE else 0o755)
        struct.pack_into('<I', itbl, off+4, size)
        for i in range(SFS_DIRECT):
            blk = direct[i] if i < len(direct) else 0
            struct.pack_into('<I', itbl, off+8+i*4, blk)
        # remaining indirect/dindirect/tindirect/atim/mtim/ctim/nlink/flags are 0

    # root directory inode
    root_blocks = []
    root_data = bytearray()

    # Build dirents for root: ".." (to self) + each file
    # dirent: inode(4) + name[64]
    dir_entries = b''
    for name, data in all_files:
        entry = u32_le(finos[name]) + name.encode() + b'\x00' * (64 - len(name))
        dir_entries += entry

    # Dirents must be packed CONTIGUOUSLY (the kernel computes entry byte
    # offsets as index*68 and handles dirents that span block boundaries).
    # Padding is only allowed at the END of the stream, never between chunks.
    root_size = len(dir_entries)
    rblk_needed = (root_size + SFS_BLOCK_SIZE - 1) // SFS_BLOCK_SIZE
    root_data = dir_entries.ljust(rblk_needed * SFS_BLOCK_SIZE, b'\x00')
    for fi in range(rblk_needed):
        blk = ds + fi
        set_bit(block_bmap, blk)
        root_blocks.append(blk)
        image.seek(blk * SFS_BLOCK_SIZE)
        image.write(root_data[fi*SFS_BLOCK_SIZE:(fi+1)*SFS_BLOCK_SIZE])

    write_inode(0, SFS_TYPE_DIR, root_size, root_blocks)

    next_data_block = ds + rblk_needed

    # Write each file's inode and data
    for name, data in all_files:
        ino = finos[name]
        fsz = len(data)
        fblks = []
        for fi in range(0, fsz, SFS_BLOCK_SIZE):
            blk = next_data_block
            next_data_block += 1
            set_bit(block_bmap, blk)
            fblks.append(blk)
            chunk = data[fi:fi+SFS_BLOCK_SIZE]
            chunk = chunk.ljust(SFS_BLOCK_SIZE, b'\x00')
            image.seek(blk * SFS_BLOCK_SIZE)
            image.write(chunk)
        write_inode(ino, SFS_TYPE_FILE, fsz, fblks)

    # Write block bitmap
    image.seek((ibs + 1) * SFS_BLOCK_SIZE)
    image.write(block_bmap)

    # Write inode table
    image.seek((ibs + bbs + 1) * SFS_BLOCK_SIZE)
    image.write(itbl)

    # Flush to file
    with open(out_path, 'wb') as f:
        f.write(image.getvalue())

    print(f"  SFS image: {out_path} ({total_blocks} blocks, {len(all_files)} files, {usable} usable)")

def main():
    import subprocess
    build_dir = os.path.join(os.path.dirname(__file__) or '.', '..', 'build')
    out_path = os.path.join(build_dir, 'root.sfs')

    embedded = {
        'hello.elf':       os.path.join(build_dir, 'user_program.elf'),
        'cat.elf':         os.path.join(build_dir, 'cat_program.elf'),
        'hello-c.elf':     os.path.join(build_dir, 'hello-c.elf'),
        'tcp_echo.elf':    os.path.join(build_dir, 'tcp_echo-c.elf'),
        'udp_echo.elf':    os.path.join(build_dir, 'udp_echo-c.elf'),
        'thread_test.elf': os.path.join(build_dir, 'thread_test-c.elf'),
        'ld.so':           os.path.join(build_dir, 'ld.so'),
        'libdyn.so':       os.path.join(build_dir, 'libdyn.so'),
        'hello-dyn.elf':   os.path.join(build_dir, 'hello-dyn.elf'),
    }

    # Text boot files (previously created by the kernel from ramdisk)
    embedded['version.txt'] = b'OPERtur/TRY1 OS v0.2.0\nLayered x86-64 Kernel\n'
    embedded['welcome.txt'] = b'echo Welcome to OPERtur/TRY1 OS!\nversion\nls\n'

    files = {}
    for name, src in embedded.items():
        if isinstance(src, bytes):
            files[name] = src
            continue
        if not os.path.exists(src):
            print(f"  SKIP {src} (not built)")
            continue
        with open(src, 'rb') as f:
            files[name] = f.read()

    if not files:
        print("ERROR: no ELF files found, build them first")
        sys.exit(1)

    # Total blocks: 2 MB = 4096 blocks, minus 256 journal = 3840 usable. Use all 4096.
    build_sfs(files, out_path, 4096)

if __name__ == '__main__':
    main()
