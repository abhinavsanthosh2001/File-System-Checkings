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

void check_inode_types(struct dinode *dip, int ninodes) {
  int i;
  for ( i = 0; i < ninodes; i++) {
    if (dip[i].type != 0 && dip[i].type != T_FILE && dip[i].type != T_DIR && dip[i].type != T_DEV) {
        fprintf(stderr, "ERROR: bad inode.\n");
        exit(1);
    }
  }
  // printf("inode type check OK!\n");
}

void check_block_addresses(struct dinode *dip, int ninodes, int nblocks) {
  int i, j;
  for ( i = 0; i < ninodes; i++) {
      if (dip[i].type == 0) {
          continue; // Skip unallocated inodes
      }
      
      // Check direct block addresses
      for ( j = 0; j < NDIRECT; j++) {
          if (dip[i].addrs[j] != 0 && (dip[i].addrs[j] < 0 || dip[i].addrs[j] >= nblocks)) {
              fprintf(stderr, "ERROR: bad direct address in inode.\n");
              exit(1);
          }
      }
      
      // Check indirect block address
      if (dip[i].addrs[NDIRECT] != 0 && (dip[i].addrs[NDIRECT] < 0 || dip[i].addrs[NDIRECT] >= nblocks)) {
          fprintf(stderr, "ERROR: bad indirect address in inode.\n");
          exit(1);
      }
  }
  // printf("check_block_addresses OK!\n");
}

void check_root_directory(struct dinode *dip, struct dirent *de) {
    int pfound = 0;
    int j;
    for ( j = 0; j < DPB; j++, de++) {
        if (strcmp("..", de->name) == 0) {
            pfound = 1;
            if (de->inum != ROOTINO) {
                fprintf(stderr, "ERROR: root directory does not exist.\n");
                exit(1);
            }
            break;
        }
    }
    if (!pfound) {
        fprintf(stderr, "ERROR: root directory does not exist.\n");
        exit(1);
    }
}

void check_directory_format(struct dinode *dip, int ninodes, char *addr) {
    int i,j,inum;
    for ( inum = 1; inum < ninodes; inum++) {
        struct dinode *inode = &dip[inum];
        if (inode->type != T_DIR) continue;

        int pfound = 0, cfound = 0;
        for ( i = 0; i < NDIRECT; i++) {
            uint blockaddr = inode->addrs[i];
            if (blockaddr == 0) continue;

            struct dirent *de = (struct dirent *)(addr + blockaddr * BLOCK_SIZE);
            for ( j = 0; j < DPB; j++, de++) {
                if (!cfound && strcmp(".", de->name) == 0) {
                    cfound = 1;
                    if (de->inum != inum) {
                        fprintf(stderr, "ERROR: directory not properly formatted.\n");
                        exit(1);
                    }
                }
                if (!pfound && strcmp("..", de->name) == 0) {
                    pfound = 1;
                    if ((inum != ROOTINO && de->inum == inum) || (inum == ROOTINO && de->inum != inum)) {
                        fprintf(stderr, "ERROR: root directory does not exist.\n");
                        exit(1);
                    }
                }
                if (pfound && cfound) break;
            }
            if (pfound && cfound) break;
        }
        if (!pfound || !cfound) {
            fprintf(stderr, "ERROR: directory not properly formatted.\n");
            exit(1);
        }
    }
}

void check_block_usage_in_bitmap(struct dinode *dip, char *bitmap, int ninodes, int nblocks, char *fs_img) {
  int i, j, k;
  for (i = 0; i < ninodes; i++) {
    if (dip[i].type == 0) {
      continue; // Skip unallocated inodes
    }

    // Check direct blocks
    for (j = 0; j < NDIRECT; j++) {
      if (dip[i].addrs[j] != 0) {
        int block_num = dip[i].addrs[j];
        if (block_num < 0 || block_num >= nblocks || !(bitmap[block_num / 8] & (1 << (block_num % 8)))) {
          fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
          exit(1);
        }
      }
    }

    // Check indirect blocks
    if (dip[i].addrs[NDIRECT] != 0) {
      int block_num = dip[i].addrs[NDIRECT];
      if (block_num < 0 || block_num >= nblocks || !(bitmap[block_num / 8] & (1 << (block_num % 8)))) {
        fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
        exit(1);
      }

      // Check blocks referenced by the indirect block
      uint *indirect_block = (uint *)(fs_img + block_num * BLOCK_SIZE);
      for (k = 0; k < NINDIRECT; k++) {
        if (indirect_block[k] != 0) {
          int indir_block_num = indirect_block[k];
          if (indir_block_num < 0 || indir_block_num >= nblocks || !(bitmap[indir_block_num / 8] & (1 << (indir_block_num % 8)))) {
            fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
            exit(1);
          }
        }
      }
    }
  }
  // printf("check_block_usage_in_bitmap OK!\n");
}

void check_bitmap_consistency_with_inodes(struct dinode *dip, char *bitmap, int ninodes, int nblocks, void *img_ptr) {
    int i, j, k;
    uint *indirect;

    // Iterate over all inodes
    for (i = 0; i < ninodes; i++) {
        if (dip[i].type == 0) {
            continue; // Skip free inodes
        }

        // Check direct blocks
        for (j = 0; j < NDIRECT; j++) {
            if (dip[i].addrs[j] != 0) {
                if (!is_block_in_use(dip[i].addrs[j], bitmap)) {
                    printf("ERROR: bitmap marks block in use but it is not in use.\n");
                    return;
                }
            }
        }

        // Check indirect blocks
        if (dip[i].addrs[NDIRECT] != 0) {
            indirect = (uint *)(img_ptr + dip[i].addrs[NDIRECT] * BSIZE);
            for (k = 0; k < NINDIRECT; k++) {
                if (indirect[k] != 0) {
                    if (!is_block_in_use(indirect[k], bitmap)) {
                        printf("ERROR: bitmap marks block in use but it is not in use.\n");
                        return;
                    }
                }
            }
        }
    }
    // printf("check_bitmap_consistency_with_inodes OK!\n");


}

int is_block_in_use(uint block, char *bitmap) {
    uint block_index = block / 8;
    uint block_offset = block % 8;
    return (bitmap[block_index] & (1 << block_offset)) != 0;
}

void check_direct_address_uniqueness(struct dinode *dip, int ninodes, int nblocks, char *addr) {
    uint duaddrs[nblocks];
    memset(duaddrs, 0, sizeof(uint) * nblocks);
    int i,inum;


    for ( inum = 1; inum < ninodes; inum++) {
        struct dinode *inode = &dip[inum];
        if (inode->type == 0) continue;

        for ( i = 0; i < NDIRECT; i++) {
            uint blockaddr = inode->addrs[i];
            if (blockaddr == 0) continue;
            duaddrs[blockaddr]++;
        }
    }

    for ( i = 0; i < nblocks; i++) {
        if (duaddrs[i] > 1) {
            fprintf(stderr, "ERROR: direct address used more than once.\n");
            exit(1);
        }
    }
}

void check_indirect_address_uniqueness(struct dinode *dip, int ninodes, int nblocks, char *addr) {
    uint iuaddrs[nblocks];
    memset(iuaddrs, 0, sizeof(uint) * nblocks);
    int i,inum;

    for ( inum = 1; inum < ninodes; inum++) {
        struct dinode *inode = &dip[inum];
        if (inode->type == 0) continue;

        uint blockaddr = inode->addrs[NDIRECT];
        if (blockaddr == 0) continue;

        uint *indirect = (uint *)(addr + blockaddr * BLOCK_SIZE);
        for ( i = 0; i < NINDIRECT; i++) {
            blockaddr = indirect[i];
            if (blockaddr == 0) continue;
            iuaddrs[blockaddr]++;
        }
    }

    for ( i = 0; i < nblocks; i++) {
        if (iuaddrs[i] > 1) {
            fprintf(stderr, "ERROR: indirect address used more than once.\n");
            exit(1);
        }
    }
}

// void check_inode_referred_in_directory(struct dinode *dip, int ninodes, int nblocks, char *addr) {
//     bool inode_found;
//     struct dirent *de;
//     int i, j, n, inode_num;

//     // Iterate through inodes
//     for ( inode_num = 1; inode_num < ninodes; inode_num++) {
//         if (dip[inode_num].type == 0) {
//             continue; // Skip unallocated inodes
//         }

//         inode_found = false;

//         // Check all directories for references to the inode
//         for (i = 0; i < ninodes; i++) {
//             if (dip[i].type == T_DIR) { // Check only directories
//                 // Read the directory entries
//                 de = (struct dirent *)(addr + dip[i].addrs[0] * BLOCK_SIZE);
//                 n = dip[i].size / sizeof(struct dirent);

//                 for ( j = 0; j < n; j++, de++) {
//                     if (de->inum == inode_num) {
//                         inode_found = true;
//                         break;
//                     }
//                 }
//                 if (inode_found) {
//                     break;
//                 }
//             }
//         }

//         // If inode is not found in any directory, report error
//         if (!inode_found) {
//             fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
//             exit(1);
//     }
//   }
//     // printf("check_inode_referred_in_directory OK!\n");

// }

// void check_inode_referred_in_directory_marked_in_use(struct dinode *dip, int ninodes, int nblocks, char *addr) {
//     struct dirent *de;
//     int i, j, n, inode_num;

//     // Iterate through directories to check inode references
//     for ( inode_num = 1; inode_num < ninodes; inode_num++) {
//         if (dip[inode_num].type == 0) {
//             continue; // Skip unallocated inodes
//         }

//         // Check all directories for references to the inode
//         for (i = 0; i < ninodes; i++) {
//             if (dip[i].type == T_DIR) { // Check only directories
//                 // Read the directory entries
//                 de = (struct dirent *)(addr + dip[i].addrs[0] * BLOCK_SIZE);
//                 n = dip[i].size / sizeof(struct dirent);

//                 for ( j = 0; j < n; j++, de++) {
//                     if (de->inum == inode_num) {
//                         // If the inode is referred to but is not in use, report error
//                         if (dip[inode_num].type == 0) {
//                             fprintf(stderr, "ERROR: inode referred to in directory but marked free.\n");
//                             exit(1);
//                         }
//                     }
//                 }
//             }
//     }
//   }
//     // printf("check_inode_referred_in_directory_marked_in_use OK!\n");

// }

// void check_reference_count_for_files(struct dinode *dip, int ninodes, int nblocks, char *addr) {
//     struct dirent *de;
//     int i, j, n, inode_num;

//     // Iterate through all inodes
//     for (inode_num = 1; inode_num < ninodes; inode_num++) {
//         if (dip[inode_num].type == T_FILE) { // Only check regular files
//             int link_count = 0; // Variable to count directory references for the file

//             // Check all directories for references to the inode
//             for (i = 0; i < ninodes; i++) {
//                 if (dip[i].type == T_DIR) { // Check only directories
//                     // Read the directory entries
//                     de = (struct dirent *)(addr + dip[i].addrs[0] * BLOCK_SIZE);
//                     n = dip[i].size / sizeof(struct dirent);

//                     // Count how many times the inode is referred to in directories
//                     for ( j = 0; j < n; j++, de++) {
//                         if (de->inum == inode_num) {
//                             link_count++;
//                         }
//                     }
//                 }
//             }

//             // Check if the link count matches the inode's link count
//             if (link_count != dip[inode_num].nlink) {
//                 fprintf(stderr, "ERROR: bad reference count for file.\n");
//                 exit(1);
//             }
//   }
//  }
//     //  printf("check_reference_count_for_files OK!\n");

// }

// void check_directory_links(struct dinode *dip, int ninodes, int nblocks, char *addr) {
//     struct dirent *de;
//     int i, j, n, inode_num;
//     int *directory_in_use = calloc(ninodes, sizeof(int)); // Use calloc to initialize to zero
//     if (!directory_in_use) {
//         fprintf(stderr, "Memory allocation failed\n");
//         exit(1);
//     }

//     // First pass: count external references to directories
//     for (i = 0; i < ninodes; i++) {
//         if (dip[i].type == T_DIR) {
//             de = (struct dirent *)(addr + dip[i].addrs[0] * BLOCK_SIZE);
//             n = dip[i].size / sizeof(struct dirent);

//             for (j = 0; j < n; j++, de++) {
//                 if (de->inum != 0 && strcmp(de->name, ".") != 0 && strcmp(de->name, "..") != 0) {
//                     if (dip[de->inum].type == T_DIR) {
//                         directory_in_use[de->inum]++;
//                     }
//                 }
//             }
//         }
//     }

//     // Second pass: check for multiple references
//     for (inode_num = 1; inode_num < ninodes; inode_num++) {
//         if (dip[inode_num].type == T_DIR && inode_num != ROOTINO) {
//             if (directory_in_use[inode_num] > 1) {
//                 fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
//                 free(directory_in_use);
//                 exit(1);
//             }
//             if (directory_in_use[inode_num] == 0) {
//                 fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
//                 free(directory_in_use);
//                 exit(1);
//             }
//         }
//     }

//     free(directory_in_use);
//     // printf("check_directory_links OK!\n");
// }

void traverse_dirs(char *addr, struct dinode *rootinode, int *inodemap, struct dinode *dip) {
    int i, j;
    uint blockaddr;
    uint *indirect;
    struct dinode *inode;
    struct dirent *dir;

    if (rootinode->type == T_DIR) {
        // Traverse direct addresses
        for (i = 0; i < NDIRECT; i++) {
            blockaddr = rootinode->addrs[i];
            if (blockaddr == 0) continue;

            dir = (struct dirent *)(addr + blockaddr * BLOCK_SIZE);
            for (j = 0; j < DPB; j++, dir++) {
                if (dir->inum != 0 && strcmp(dir->name, ".") != 0 && strcmp(dir->name, "..") != 0) {
                    inode = &dip[dir->inum];
                    inodemap[dir->inum]++;
                    // Recursion
                    traverse_dirs(addr, inode, inodemap, dip);
                }
            }
        }

        // Traverse indirect addresses
        blockaddr = rootinode->addrs[NDIRECT];
        if (blockaddr != 0) {
            indirect = (uint *)(addr + blockaddr * BLOCK_SIZE);
            for (i = 0; i < NINDIRECT; i++, indirect++) {
                blockaddr = *(indirect);
                if (blockaddr == 0) continue;

                dir = (struct dirent *)(addr + blockaddr * BLOCK_SIZE);
                for (j = 0; j < DPB; j++, dir++) {
                    if (dir->inum != 0 && strcmp(dir->name, ".") != 0 && strcmp(dir->name, "..") != 0) {
                        inode = &dip[dir->inum];
                        inodemap[dir->inum]++;
                        // Recursion
                        traverse_dirs(addr, inode, inodemap, dip);
                    }
                }
            }
        }
    }
}

void directory_check(struct dinode *dip, int ninodes, int nblocks, char *addr) {
    int inodemap[ninodes];
    memset(inodemap, 0, sizeof(int) * ninodes);
    struct dinode *inode, *rootinode;
    int i;

    inode = dip;
    rootinode = ++inode;
    inodemap[0]++;
    inodemap[1]++;

    // Traverse all directories and count how many times each inode number has been referred by directory
    traverse_dirs(addr, rootinode, inodemap, dip);
    inode+=1;

    // Go through all inodes to check rules 9-12
    for (i = 1; i < ninodes; i++) {
        inode = &dip[i];
        // Rule 9
        if (inode->type != 0 && inodemap[i] == 0) {
            fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
            exit(1);
        }

        // Rule 10
        if (inodemap[i] > 0 && inode->type == 0) {
            fprintf(stderr, "ERROR: inode referred to in directory but marked free.\n");
            exit(1);
        }

        // Rule 11
        // Reference count check for all files
        if (inode->type == T_FILE && inode->nlink != inodemap[i]) {
            fprintf(stderr, "ERROR: bad reference count for file.\n");
            exit(1);
        }

        // Rule 12
        if (inode->type == T_DIR && inodemap[i] > 1) {
            fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
            exit(1);
        }
    }
}
int
main(int argc, char *argv[])
{
  int r,i,n,fsfd;
  char *addr;
  struct dinode *dip;
  struct superblock *sb;
  struct dirent *de;

  if(argc < 2){
    fprintf(stderr, "Usage: sample fs.img ...\n");
    exit(1);
  }


  fsfd = open(argv[1], O_RDONLY);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  /* Dont hard code the size of file. Use fstat to get the size */
  addr = mmap(NULL, 524248, PROT_READ, MAP_PRIVATE, fsfd, 0);
  if (addr == MAP_FAILED){
	perror("mmap failed");
	exit(1);
  }
  /* read the super block */
  sb = (struct superblock *) (addr + 1 * BLOCK_SIZE);
//   printf("fs size %d, no. of blocks %d, no. of inodes %d \n", sb->size, sb->nblocks, sb->ninodes);

  /* read the inodes */
  dip = (struct dinode *) (addr + IBLOCK((uint)0)*BLOCK_SIZE); 
//   printf("begin addr %p, begin inode %p , offset %d \n", addr, dip, (char *)dip -addr);

//   // read root inode
//   printf("Root inode  size %d links %d type %d \n", dip[ROOTINO].size, dip[ROOTINO].nlink, dip[ROOTINO].type);

  // get the address of root dir 
  de = (struct dirent *) (addr + (dip[ROOTINO].addrs[0])*BLOCK_SIZE);

  // print the entries in the first block of root dir 

  n = dip[ROOTINO].size/sizeof(struct dirent);
//   for (i = 0; i < n; i++,de++){
//  	printf(" inum %d, name %s ", de->inum, de->name);
//   	printf("inode  size %d links %d type %d \n", dip[de->inum].size, dip[de->inum].nlink, dip[de->inum].type);
//   }

  check_inode_types(dip, sb->ninodes);
  check_block_addresses(dip, sb->ninodes, sb->nblocks);
  check_root_directory(dip, (struct dirent *)(addr + (dip[ROOTINO].addrs[0])*BLOCK_SIZE));
  check_directory_format(dip, sb->ninodes, addr);
  check_block_usage_in_bitmap(dip, addr + BBLOCK(0, sb->ninodes) * BLOCK_SIZE, sb->ninodes, sb->nblocks, addr);
  check_bitmap_consistency_with_inodes(dip, addr + BBLOCK(0, sb->ninodes) * BLOCK_SIZE, sb->ninodes, sb->nblocks, addr);
  check_direct_address_uniqueness(dip, sb->ninodes, sb->nblocks,addr);
  check_indirect_address_uniqueness(dip, sb->ninodes, sb->nblocks, addr);
//   check_inode_referred_in_directory(dip, sb->ninodes, sb->nblocks, addr);
//   check_inode_referred_in_directory_marked_in_use(dip, sb->ninodes, sb->nblocks, addr);
//   check_reference_count_for_files(dip, sb->ninodes, sb->nblocks, addr);
//   check_directory_links(dip, sb->ninodes, sb->nblocks, addr);
  directory_check(dip, sb->ninodes, sb->nblocks, addr);

  exit(0);

}

