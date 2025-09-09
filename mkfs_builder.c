// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u






uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct {
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");// this is to make sure superblock is 116 or gives error

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR INODE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;// (access time) field records the last time the file was accessed (read or executed).    uint64_t mtime;
    uint64_t mtime;// this records the last time the file's content was modified (written to).
    uint64_t ctime;// last time the inode itself was modified (e.g., changes to metadata like permissions or ownership).
    uint32_t direct[12]; // direct pointers to data blocks
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id; //group id
    uint32_t uid16_gid16; // lower 16 bits of uid and gid
    uint64_t xattr_ptr;
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR DIRECTORY ENTRY STRUCTURE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE
    uint32_t inode_no;
    uint8_t type; // 1=file, 2=dir
    char name[58];


    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t; // this is a directory entry's structure
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

int main( int argc, char *argv[]) {
    crc32_init();
    time_t now = time(NULL);
    // WRITE YOUR DRIVER CODE HERE
    // PARSE YOUR CLI PARAMETERS
    char *image_name = NULL;
    uint64_t size_kib = 0;
    uint64_t inode_count = 0;
    uint64_t total_blocks = 0;

    //for loop for argument parsing:
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            if(argv[i+1]!=NULL){
                image_name = argv[++i]; //argument after --image is the image name and assined here
            } else {
                fprintf(stderr, "Error: --image requires a non-null argument.\n");
                return 1;
            } // check if argument after --image is not null
            
        } 
        else if (strcmp(argv[i], "--size-kib") == 0 && i + 1 < argc) {
            size_kib = strtoull(argv[++i], NULL, 10); //argument, dont need ignore, base
        } 
        else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc) {
            inode_count = strtoull(argv[++i], NULL, 10);
        } 
        else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            return 1;
        }
    }



    if (!image_name) {
        fprintf(stderr, "Error: --image is required.\n");
        return 1;
    }
    if (size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0) {
        fprintf(stderr, "Error: --size-kib must be between 180 and 4096 and a multiple of 4.\n");
        return 1;
    }
    if (inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Error: --inodes must be between 128 and 512.\n");
        return 1;
    }

    // THEN CREATE YOUR FILE SYSTEM WITH A ROOT DIRECTORY
    total_blocks = size_kib*1024/BS;
    superblock_t sb;
    memset(&sb, 0, sizeof(sb)); // clear all bytes of superblick var
    sb.magic = 0x4D565346;
    sb.total_blocks = total_blocks;
    sb.inode_count = inode_count;
    sb.version = 1;
    sb.block_size = BS;
    sb.root_inode = 1;
    sb.mtime_epoch = now;
    sb.flags = 0;
    sb.inode_bitmap_start = 1;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = 2;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = 3;
    sb.inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    sb.data_region_start = sb.inode_table_start + sb.inode_table_blocks;
    sb.data_region_blocks = sb.total_blocks - sb.data_region_start;

    // Use a full BS-sized buffer when finalizing CRC 
    uint8_t sb_block[BS];
    memset(sb_block, 0, BS); // init w 0
    memcpy(sb_block, &sb, sizeof(sb)); //sb struct into a blocksize buffer
    sb.checksum = superblock_crc_finalize((superblock_t*)sb_block); // make sb block a superblock ptr and finalize crc

    

    //inode bitmap
    uint64_t inode_bitmap_bytes = (inode_count + 7) / 8;// num of bytes needed in the bitmap according to inode count
    uint8_t *inode_bitmap = calloc(inode_bitmap_bytes, 1); //num of bytes and size of each byte
    if (!inode_bitmap) {
        fprintf(stderr, "Failed to allocate inode bitmap\n");
        return 1;
    }
    

    //data bitmap
    uint64_t data_bitmap_bytes = (sb.data_region_blocks + 7) / 8;
    uint8_t *data_bitmap = calloc(data_bitmap_bytes, 1);
    if (!data_bitmap) {
        fprintf(stderr, "Failed to allocate data bitmap\n");
        return 1;
    }


    inode_t *inode_table = calloc(inode_count, sizeof(inode_t));
    if (!inode_table) {
        fprintf(stderr, "Failed to allocate inode table\n");
        return 1;
    }
    inode_table[0].mode = (0040000); //directory representation, since we are making root director
    inode_table[0].links = 2; 
    inode_table[0].uid = 0;
    inode_table[0].gid = 0;
    inode_table[0].atime = now;
    inode_table[0].mtime = now;
    inode_table[0].ctime = now;

    inode_table[0].reserved_0 = 0;
    inode_table[0].reserved_1 = 0;
    inode_table[0].reserved_2 = 0;
    inode_table[0].proj_id = 8;
    inode_table[0].uid16_gid16 = 0;
    inode_table[0].xattr_ptr = 0;
    inode_table[0].direct[0] = sb.data_region_start; // first block in data region
    inode_table[0].size_bytes = 2 * sizeof(dirent64_t);
    inode_crc_finalize(&inode_table[0]); 


    inode_bitmap[0] |= 1; // mark root inode as used | makes sure other bits staysame
    uint32_t block_index = inode_table[0].direct[0] - sb.data_region_start; 
    data_bitmap[block_index / 8] |= 1 << (block_index % 8); //which byte in bitmap-> which bit in bye-> set it to 1 and preserve other bits




    dirent64_t root_entries[2];
    memset(root_entries, 0, sizeof(root_entries));
    //. entry
    root_entries[0].inode_no = ROOT_INO;   // points to itself
    root_entries[0].type = 2;              // directory type
    strncpy(root_entries[0].name, ".", 1);
    dirent_checksum_finalize(&root_entries[0]);
    
    // ".." entry
    root_entries[1].inode_no = ROOT_INO;   // points to itself
    root_entries[1].type = 2;              // directory type
    strncpy(root_entries[1].name, "..", 2);
    dirent_checksum_finalize(&root_entries[1]);

    // THEN SAVE THE DATA INSIDE THE OUTPUT IMAGE
    FILE *fp = fopen(image_name, "wb");  //writing binary
    if (!fp) {
        perror("fopen");
        return 1;
    }
    
    fwrite(sb_block, 1, BS, fp);
    
    uint8_t inode_bitmap_block[BS] = {0}; //set a bitmap of  BS size incase inode numbers are too small as instuctions require one block to be alloced
    memcpy(inode_bitmap_block, inode_bitmap, inode_bitmap_bytes);//copy the inode bitmpa into the blc
    fseek(fp, sb.inode_bitmap_start*BS, SEEK_SET);
    fwrite(inode_bitmap_block, 1, BS, fp);
    
    uint8_t data_bitmap_block[BS] = {0};
    memcpy(data_bitmap_block, data_bitmap, data_bitmap_bytes);
    fseek(fp, sb.data_bitmap_start*BS, SEEK_SET);
    fwrite(data_bitmap_block, 1, BS, fp);

    fseek(fp, sb.inode_table_start * BS, SEEK_SET);
    fwrite(inode_table, sizeof(inode_t), inode_count, fp);
    // Write root directory entries into first data block
    fseek(fp, sb.data_region_start * BS, SEEK_SET);  // jump to data block
    fwrite(root_entries, sizeof(root_entries), 1, fp);

    uint64_t written_bytes = ftell(fp); // current position in file
    uint64_t total_bytes = total_blocks * BS; // expected total image size

    if (written_bytes < total_bytes) {
        uint64_t remaining = total_bytes - written_bytes;
        uint8_t *zeros = calloc(1, BS);
        if (!zeros) { perror("calloc"); fclose(fp); return 1; }
        while (remaining >= BS) {
            fwrite(zeros, 1, BS, fp);
            remaining -= BS;
        }
        if (remaining > 0) fwrite(zeros, 1, remaining, fp);
        free(zeros);
    }

    fclose(fp);

    free(inode_bitmap);
    free(data_bitmap);
    free(inode_table);

    return 0;
}
