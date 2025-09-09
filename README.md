# MiniVSFS Tools

This repository contains tools for creating and managing MiniVSFS (Mini Virtual Simple File System) images.

## Tools

1. **mkfs_builder** – Builds a new MiniVSFS image from scratch.
2. **mkfs_adder** – Adds a file into an existing MiniVSFS image, updating inodes, bitmaps, and directory entries.

## Usage

### Build image
```bash
./mkfs_builder --output <image.img> --size <blocks>
Add file to image
bash
Copy code
./mkfs_adder --input <image.img> --output <new_image.img> --file <hostfile>
Features
Handles inode and data block allocation automatically.

Updates inode bitmap, data bitmap, directory entries, and CRC checks.

Supports files up to DIRECT_MAX * BS blocks.


