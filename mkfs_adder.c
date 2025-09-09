// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push, 1)
typedef struct {
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
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type; // 1=file, 2=dir
    char name[58];
    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
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
// ==========================DO NOT CHANGE THIS PORTION=========================


static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s --input <in.img> --output <out.img> --file <hostfile>\n", prog);
    exit(1);
}

static int find_first_zero_bit(uint8_t *bitmap, uint64_t bits) {
    // returns 0-based index of first zero bit, or -1 if none
    uint64_t bytes = (bits + 7) / 8;
    for (uint64_t b = 0; b < bytes; b++) {
        if (bitmap[b] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                uint64_t idx = b*8 + bit;
                if (idx >= bits) return -1;
                if ((bitmap[b] & (1u << bit)) == 0) return (int)idx;
            }
        }
    }
    return -1;
}

static int get_bit(uint8_t *bitmap, uint64_t idx) {
    return (bitmap[idx/8] >> (idx%8)) & 1;
}
static void set_bit(uint8_t *bitmap, uint64_t idx) {
    bitmap[idx/8] |= (1u << (idx%8));
}
static void clear_bit(uint8_t *bitmap, uint64_t idx) {
    bitmap[idx/8] &= ~(1u << (idx%8));
}

int main(int argc, char *argv[]) {
    crc32_init();

    char *input = NULL;
    char *output = NULL;
    char *hostfile = NULL;

    if (argc < 7) usage(argv[0]);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i],"--input")==0 && i+1<argc) input = argv[++i];
        else if (strcmp(argv[i],"--output")==0 && i+1<argc) output = argv[++i];
        else if (strcmp(argv[i],"--file")==0 && i+1<argc) hostfile = argv[++i];
        else { fprintf(stderr,"Unknown arg: %s\n", argv[i]); usage(argv[0]); }
    }
    if (!input || !output || !hostfile) usage(argv[0]);

    // open input image and read superblock block first
    FILE *fin = fopen(input, "rb");
    if (!fin) { perror("fopen input"); return 1; }

    uint8_t sb_block[BS];
    if (fread(sb_block, 1, BS, fin) != BS) { fprintf(stderr,"Failed to read superblock block\n"); fclose(fin); return 1; }

    // interpret superblock from sb_block
    superblock_t *sbp = (superblock_t*)sb_block;
    if (sbp->magic != 0x4D565346u) { fprintf(stderr,"Bad magic: 0x%08x\n", sbp->magic); fclose(fin); return 1; }
    // copy to local sb (we will modify fields only via sb_block when finalizing)
    superblock_t sb;
    memcpy(&sb, sbp, sizeof(superblock_t));

    // now read entire image into memory (size = sb.total_blocks * BS)
    uint64_t total_blocks = sb.total_blocks;
    if (total_blocks == 0) { fprintf(stderr, "Invalid total_blocks\n"); fclose(fin); return 1; }
    uint64_t image_size = total_blocks * (uint64_t)BS;

    // allocate buffer and read whole file
    uint8_t *img = malloc(image_size);
    if (!img) { perror("malloc image"); fclose(fin); return 1; }
    // we already read first block into sb_block; copy it
    memcpy(img, sb_block, BS);
    // read the rest
    size_t toread = image_size - BS;
    if (toread > 0) {
        if (fread(img + BS, 1, toread, fin) != toread) { fprintf(stderr,"Failed to read full image\n"); free(img); fclose(fin); return 1; }
    }
    fclose(fin);

    // locate bitmaps/inode table/data region offsets
    uint8_t *inode_bitmap_block = img + sb.inode_bitmap_start * BS;
    uint8_t *data_bitmap_block = img + sb.data_bitmap_start * BS;
    uint8_t *inode_table_block = img + sb.inode_table_start * BS;
    uint8_t *data_region_block = img + sb.data_region_start * BS;

    uint64_t inode_count = sb.inode_count;
    uint64_t data_region_blocks = sb.data_region_blocks;

    // compute number of bytes in bitmaps (they were stored as full blocks by builder)
    // but builder wrote full block; still, use the block pointer and treat it as a block-sized bitmap.
    uint8_t *inode_bitmap = inode_bitmap_block; // BS-sized area
    uint8_t *data_bitmap = data_bitmap_block;   // BS-sized area

    // locate inode table as array
    inode_t *inode_table = (inode_t*) inode_table_block;

    // read host file content
    FILE *fh = fopen(hostfile, "rb");
    if (!fh) { perror("open host file"); free(img); return 1; }
    if (fseek(fh, 0, SEEK_END) != 0) { perror("fseek"); fclose(fh); free(img); return 1; }
    long host_size_l = ftell(fh);
    if (host_size_l < 0) { perror("ftell"); fclose(fh); free(img); return 1; }
    uint64_t host_size = (uint64_t)host_size_l;
    rewind(fh);

    if (host_size > DIRECT_MAX * (uint64_t)BS) {
        fprintf(stderr, "File too large: max %u bytes\n", DIRECT_MAX * BS);
        fclose(fh); free(img); return 1;
    }

    // find free inode (first-fit)
    int free_inode_bit = find_first_zero_bit(inode_bitmap, inode_count);
    if (free_inode_bit < 0) {
        fprintf(stderr, "No free inode available\n"); fclose(fh); free(img); return 1;
    }
    uint64_t inode_index = (uint64_t)free_inode_bit; // 0-based index into table
    uint64_t inode_no = inode_index + 1; // 1-based inode number

    // compute how many data blocks needed
    uint64_t need_blocks = (host_size + BS - 1) / BS;
    if (need_blocks == 0) need_blocks = 1; // zero-length file -> still occupy 1 block?
    if (need_blocks > DIRECT_MAX) { fprintf(stderr,"File requires too many blocks (>12)\n"); fclose(fh); free(img); return 1; }

    // find free data blocks (first-fit, allocate any free ones)
    uint32_t allocated_blocks[DIRECT_MAX];
    uint64_t allocated = 0;
    uint64_t total_data_blocks = sb.data_region_blocks;
    for (uint64_t db = 0; db < total_data_blocks && allocated < need_blocks; db++) {
        if (get_bit(data_bitmap, db) == 0) {
            allocated_blocks[allocated++] = (uint32_t)db; // store relative index within data region
        }
    }
    if (allocated < need_blocks) {
        fprintf(stderr, "Not enough free data blocks (%" PRIu64 " available, %" PRIu64 " needed)\n", allocated, need_blocks);
        fclose(fh); free(img); return 1;
    }

    // write file data into allocated blocks (absolute block numbers = sb.data_region_start + rel)
    uint8_t file_block[BS];
    size_t remaining = host_size;
    for (uint64_t i = 0; i < need_blocks; i++) {
        size_t toreadblock = remaining > BS ? BS : (size_t)remaining;
        memset(file_block, 0, BS);
        if (toreadblock > 0) {
            if (fread(file_block, 1, toreadblock, fh) != toreadblock) { perror("read host file"); fclose(fh); free(img); return 1; }
        }
        uint64_t rel = allocated_blocks[i];
        uint64_t abs_block = sb.data_region_start + rel;
        memcpy(img + abs_block * BS, file_block, BS);
        remaining -= toreadblock;
        // mark data bitmap bit (relative index)
        set_bit(data_bitmap, rel);
    }
    fclose(fh);

    // update inode bitmap: mark inode_index as used
    set_bit(inode_bitmap, inode_index);

    // fill inode fields for new inode
    inode_t new_ino;
    memset(&new_ino, 0, sizeof(inode_t));
    new_ino.mode = 0100000u; // regular file (octal)
    new_ino.links = 1;
    new_ino.uid = 0;
    new_ino.gid = 0;
    new_ino.size_bytes = host_size;
    time_t now = time(NULL);
    new_ino.atime = (uint64_t)now;
    new_ino.mtime = (uint64_t)now;
    new_ino.ctime = (uint64_t)now;
    for (int i = 0; i < DIRECT_MAX; i++) new_ino.direct[i] = 0;
    for (uint64_t i = 0; i < need_blocks; i++) {
        uint64_t rel = allocated_blocks[i];
        uint64_t abs_block = sb.data_region_start + rel;
        new_ino.direct[i] = (uint32_t)abs_block;
    }
    new_ino.reserved_0 = new_ino.reserved_1 = new_ino.reserved_2 = 0;
    new_ino.proj_id = 8;
    new_ino.uid16_gid16 = 0;
    new_ino.xattr_ptr = 0;
    inode_crc_finalize(&new_ino);

    // write new inode into inode_table (in-memory img)
    inode_table[inode_index] = new_ino;

    // Add directory entry into root directory's first data block
    uint64_t root_abs_block = inode_table[0].direct[0]; // root's first data block (absolute)
    if (root_abs_block == 0) { fprintf(stderr,"Root has no data block\n"); free(img); return 1; }
    uint8_t *root_block_ptr = img + root_abs_block * BS;

    // Find free dirent slot in the first block (each dirent is 64 bytes)
    dirent64_t *dent = (dirent64_t*)root_block_ptr;
    int found_slot = -1;
    int max_dirents = BS / sizeof(dirent64_t);
    for (int i = 0; i < max_dirents; i++) {
        if (dent[i].inode_no == 0) { found_slot = i; break; }
    }
    if (found_slot == -1) {
        fprintf(stderr, "No free directory entry in root\n"); free(img); return 1;
    }

    // prepare dirent for new file
    dirent64_t new_de;
    memset(&new_de, 0, sizeof(dirent64_t));
    new_de.inode_no = (uint32_t)inode_no;
    new_de.type = 1; // file
    // get basename of hostfile to use as name (strip directories)
    const char *basename = hostfile;
    const char *p = strrchr(hostfile, '/');
    if (p) basename = p + 1;
    // copy at most 57 bytes and leave last char as '\0' possibility (name field 58 bytes)
    strncpy(new_de.name, basename, sizeof(new_de.name)-1);
    dirent_checksum_finalize(&new_de);

    // write dirent into root block at slot
    memcpy(&dent[found_slot], &new_de, sizeof(dirent64_t));

    // update root inode: size increases by 64, links++ (spec says root links increases by 1)
    inode_table[0].size_bytes = inode_table[0].size_bytes + sizeof(dirent64_t);
    inode_table[0].links += 1;
    inode_table[0].mtime = (uint64_t)now;
    inode_table[0].ctime = (uint64_t)now;
    inode_crc_finalize(&inode_table[0]); // update root inode CRC

    // recompute superblock checksum safely and write it into img[0..BS)
    {
        uint8_t tmp_sb_block[BS];
        memset(tmp_sb_block, 0, BS);
        // copy current sb into block
        memcpy(tmp_sb_block, &sb, sizeof(superblock_t));
        // call finalize on block (DO NOT CHANGE function expects full block)
        superblock_crc_finalize((superblock_t*)tmp_sb_block);
        // copy the full block back into img
        memcpy(img, tmp_sb_block, BS);
    }

    // write modified image to output file
    FILE *fout = fopen(output, "wb");
    if (!fout) { perror("fopen output"); free(img); return 1; }
    if (fwrite(img, 1, image_size, fout) != image_size) {
        perror("write output"); fclose(fout); free(img); return 1;
    }
    fclose(fout);
    free(img);

    printf("Added file '%s' as inode %" PRIu64 " using %" PRIu64 " blocks.\n", hostfile, inode_no, need_blocks);
    return 0;
}
