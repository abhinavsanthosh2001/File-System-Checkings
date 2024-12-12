#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <stdbool.h>

#include "types.h"
#include "fs.h"

#define BLOCK_SIZE (BSIZE)

void check_bad_inodes(struct dinode *dip, int ninodes) {
    int i;
    for ( i = 0; i < ninodes; i++) {
        if (dip[i].type != 0 && dip[i].type != T_FILE && dip[i].type != T_DIR && dip[i].type != T_DEV) {
            fprintf(stderr, "ERROR: bad inode.\n");
            exit(1);
        }
    }
}

int main(int argc, char *argv[]) {
    int fsfd;
    int i,n;
    struct dirent *de;
    char *addr;
    struct dinode *dip;
    struct superblock *sb;

    if (argc < 2) {
        fprintf(stderr, "Usage: sample fs.img ...\n");
        exit(1);
    }

    fsfd = open(argv[1], O_RDONLY);
    if (fsfd < 0) {
        perror(argv[1]);
        exit(1);
    }

    struct stat st;
    if (fstat(fsfd, &st) < 0) {
        perror("fstat failed");
        exit(1);
    }

    addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fsfd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    /* read the superblock */
    sb = (struct superblock *) (addr + 1 * BLOCK_SIZE);
    printf("fs size %d, no. of blocks %d, no. of inodes %d \n", sb->size, sb->nblocks, sb->ninodes);

    /* read the inodes */
    dip = (struct dinode *) (addr + IBLOCK((uint)0) * BLOCK_SIZE);
    printf("begin addr %p, begin inode %p , offset %d \n", addr, dip, (char *)dip - addr);

    n = dip[ROOTINO].size / sizeof(struct dirent);
    de = (struct dirent *) (addr + (dip[ROOTINO].addrs[0]) * BLOCK_SIZE); // Start of root directory block

    for (i = 0; i < n; i++, de++) {
        printf("inum %d, name %s\n", de->inum, de->name);
        printf("inode size %d links %d type %d\n",
              dip[de->inum].size, dip[de->inum].nlink, dip[de->inum].type);
    }

    // Check Rule 1: Bad Inodes
    check_bad_inodes(dip, sb->ninodes);

    munmap(addr, st.st_size);
    close(fsfd);
    return 0;
}
