#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include "ext2_fs.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>

int fd;
__u32 block_size;
struct ext2_super_block *sp;

void free_summary(int bg_bitmap, int count_in_group, char *text)
{
  char buf[2000];
  // Read in 64 bits at a time
  int count_i = 0;
  // fprintf(stderr, "bg_bitmap: %d\n", bg_bitmap);
  while (count_i < count_in_group) {
    pread(fd, buf, 8, block_size * bg_bitmap + 8 * count_i/64);
    long long *bitmap = (long long *)buf;
    // fprintf(stderr, "buf: %s\n", buf);
    // fprintf(stderr, "%d\n", block_size * bg_bitmap);
    // fprintf(stderr, "%lld\n", *bitmap);
    for (int i = count_i; i < count_i + 64; i++) {
      int bit = (*bitmap >> i) & 1;
      // fprintf(stderr, "%d %d ", i, bit);
      if (bit == 0)
	printf("%s,%d\n", text, i+1);
				
      // fprintf(stderr, "\n");
    }
    count_i += 64;
  }
}

void print_date(int i_time)
{
  // Make copy to avoid pointer issues
  time_t i_time_copy = (int)i_time;
  struct tm *tm = gmtime(&i_time_copy);
  printf("%02d/%02d/%02d %02d:%02d:%02d,", tm->tm_mon+1, tm->tm_mday, \
	 tm->tm_year % 100, tm->tm_hour, tm->tm_min, tm->tm_sec);
}

void dirent(const __u32 i_block[], int parent, int len, int logical_start)
{
  char buf[2000];
  // Make copy of i_block to avoid pointer issues
  __u32 i_block_copy[len];
  for (int i = 0; i < len; i++) {
    i_block_copy[i] = i_block[i];
    //fprintf(stderr, "%d,", i_block_copy[i]);
  }
  //fprintf(stderr, "\n");
		
  for (int i = 0; i < len; i++) {
    __u32 block_num = i_block_copy[i];
    __u32 i_offset = 0;
    //fprintf(stderr, "block number: %d %d\n", i_block_copy[i], block_num);
    while (block_num != 0 && i_offset < block_size) {
      int c = pread(fd, buf, block_size, block_size * block_num + i_offset);
      if (c < 0) {
	fprintf(stderr, "Error in pread: %s\n", strerror(errno));
	exit(2);
      }
      struct ext2_dir_entry *de = (struct ext2_dir_entry *)buf;
      if (de->rec_len == 0) {
	fprintf(stderr, "Error: corrupted directory entry!\n");
	exit(2);
      }
      if (de->inode != 0) {
	printf("DIRENT,%d,", parent);
	printf("%d,", logical_start + i*block_size + i_offset);
	printf("%d,", de->inode);
	printf("%d,", de->rec_len);
	printf("%d,", de->name_len);
	char name[de->name_len + 1];
	strcpy(name, "");
	strncat(name, de->name, de->name_len);
	printf("'%s'\n", name);
      }
      i_offset += de->rec_len;
    }
  }
}

// Process indirect block to output directory entry
void dirent2(const __u32 ind_block, int parent, int level, int logical_start)
{
  // Get block numbers to process
  char buf[2000];
  if (ind_block != 0) {
    __u32 i_offset = 0;
    while (i_offset < block_size) {
      // Read 64 bits at a time
      pread(fd, buf, 8, block_size * ind_block + i_offset);
      long long *arr = (long long *)buf;
      __u32 i_block[2];
      // Go through each block contained in arr (2 blocks)
      const int SIZE = 2;
      for (int i = 0; i < SIZE; i++) {
	// Highest 32 bits of *arr will truncate automatically
	__u32 ref_block = (*arr >> 32*i);
	//fprintf(stderr, "Processing block reference: %d\n", ref_block);
	i_block[i] = ref_block;
      }
      if (level == 1)
	dirent(i_block, parent, SIZE, logical_start + block_size*SIZE*i_offset/8);
      else {
	for (int i = 0; i < 2; i++) {
	  int A = 1;
	  for (int k = 0; k < level-1; k++)
	    A *= 256; // A = 256^(level-1)
	  dirent2(i_block[i], parent, level-1,			\
		  logical_start + A*block_size*i_offset/8);
	}
      }
      i_offset += 8;
    }
  } 
}

void indirect(__u32 block_num, int level, int parent, int logical_start)
{
  char buf[2000];
  if (block_num != 0) {
    // Process 64 bits at a time
    __u32 i_offset = 0;
    while (i_offset < block_size) {
      pread(fd, buf, 8, block_size * block_num + i_offset);
      long long *arr = (long long *)buf;
      // Go through each block contained in arr (2 blocks)
      for (int i = 0; i < 2 && *arr != 0; i++) {
	// Highest 32 bits of *arr will truncate automatically
	__u32 ref_block = (*arr >> 32*i);
	if (ref_block != 0) {
	  printf("INDIRECT,%d,", parent);
	  printf("%d,", level);
	  // Compute logical block offset
	  int l_offset = logical_start + 2*i_offset/8 + i;
	  printf("%d,", l_offset);
	  printf("%d,", block_num);
	  printf("%d\n", ref_block);
	  if (level != 1)
	    indirect(ref_block, level-1, \
		     parent, l_offset);
	}
      }
      i_offset += 8;
    }
  }
}

void inode_summary(int bg_inode_table, int inodes_in_group, int s_inode_size)
{
  // fprintf(stderr, "inode size: %d\n", sp->s_inode_size);	
  char buf[2000];
  for (int i = 0; i < inodes_in_group; i++) {
    pread(fd, buf, s_inode_size, bg_inode_table * block_size + s_inode_size * i);
    struct ext2_inode *in = (struct ext2_inode *)buf;
    // fprintf(stderr, "%d ", sp->s_inode_size);
    int i_mode = in->i_mode;
    int i_links_count = in->i_links_count;
    if (i_mode != 0 && i_links_count != 0) {
      printf("INODE,%d,", i+1);
      // Print file type
      int ftype = i_mode >> 12;
      if (ftype == 0x8)
	printf("%s,", "f");
      else if (ftype == 0x4)
	printf("%s,", "d");
      else if (ftype == 0xA)
	printf("%s,", "s");
      else
	printf("%s,", "?");
				
      printf("%o,", i_mode & 0xFFF);
      printf("%d,", in->i_uid);
      printf("%d,", in->i_gid);
      printf("%d,", i_links_count);
			
      // Calculate most recent inode change
      int i_atime = in->i_atime;
      int i_ctime = in->i_ctime;
      int i_mtime = in->i_mtime;
      int i_dtime = in->i_dtime;
      int latest = i_atime > i_ctime ? i_atime : i_ctime;
      latest = i_mtime > latest ? i_mtime : latest;
      latest = i_dtime > latest ? i_dtime : latest;
      print_date(latest);
      print_date(i_mtime);
      print_date(i_atime);
			
      printf("%d,", in->i_size);
      printf("%d", in->i_blocks);
			
      // Print additional fields
      if (ftype == 0x8 || ftype == 0x4) {
	for (int i = 0; i < EXT2_N_BLOCKS; i++)
	  printf(",%d", in->i_block[i]);
      }
      // Print first four decimal bytes of name for symlinks
      else if (ftype == 0xA)
	printf(",%d", in->i_block[0]);
      printf("\n");
			
      // Save indirect blocks
      /*
      __u32 ind_block[3];
      for (int i = 0; i < 3; i++)
	ind_block[i] = in->i_block[12+i];
      */
			
      // Print directory entry
      if (ftype == 0x4) {
	dirent(in->i_block, i+1, 12, 0);
	dirent2(in->i_block[12], i+1, 1, 12*block_size);
	dirent2(in->i_block[13], i+1, 2, 268*block_size);
	dirent2(in->i_block[14], i+1, 3, 65804*block_size);
      }
      // Print indirect entries
      indirect(in->i_block[12], 1, i+1, 12);
      indirect(in->i_block[13], 2, i+1, 12+256);
      indirect(in->i_block[14], 3, i+1, 12+256+256*256);
      /*
      indirect(ind_block[0], 1, i+1, 12);
      indirect(ind_block[1], 2, i+1, 12+256);
      indirect(ind_block[2], 3, i+1, 12+256+256*256);
      */
    }		
  }
}

int main(int argc, char *argv[])
{	
  if (argc < 2) {
    fprintf(stderr, "No file name provided.\n");
    exit(1);
  }
	
  // Check for bogus arguments
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--", 2) == 0) {
      fprintf(stderr, "Option not recognized!\n");
      exit(1);
    }
  }
	
  fd = open(argv[1], O_RDONLY);
  if (fd == -1) {
    fprintf(stderr, "File not found!\n");
    exit(1);
  }

  char buf[2000];
  pread(fd, buf, 1024, 1024);
  sp = (struct ext2_super_block *)buf;
  int s_blocks_per_group = sp->s_blocks_per_group;
  int s_inodes_per_group = sp->s_inodes_per_group;
  int s_inode_size = sp->s_inode_size;
  int s_inodes_in_group = sp->s_inodes_per_group;
  int s_blocks_count = sp->s_blocks_count;
  int s_inodes_count = sp->s_inodes_count;
	
  // Print superblock summary
  printf("SUPERBLOCK,");
  printf("%d,", sp->s_blocks_count);
  printf("%d,", sp->s_inodes_count);
  block_size = EXT2_MIN_BLOCK_SIZE << sp->s_log_block_size;
  printf("%d,", block_size);
  printf("%d,", sp->s_inode_size);
  printf("%d,", sp->s_blocks_per_group);
  printf("%d,", sp->s_inodes_per_group);
  printf("%d\n", sp->s_first_ino);
	
  // fprintf(stderr, "first: %d\n", sp->s_first_data_block);
	
  // Print group summary
  int block_max_i = (int)(sp->s_blocks_count/sp->s_blocks_per_group);
  for (int i = 0; i <= block_max_i; i++) {
    printf("GROUP,%d,", i);
    // Number of blocks (inodes) in group is equal to 
    // ...sp->s_blocks_per_group (sp_s_inodes_per_group)
    // ...except in last group, which contains remaining blocks
    int blocks_in_group = s_blocks_per_group;
    int inodes_in_group = s_inodes_per_group;
    if (i == block_max_i) {
      blocks_in_group = s_blocks_count - \
	s_blocks_per_group * i;
      inodes_in_group = s_inodes_count - \
	s_inodes_per_group * i;
    }
    printf("%d,%d,", blocks_in_group, inodes_in_group);
		
    // Read block group descriptor table
    pread(fd, buf, 32, 2048);
    struct ext2_group_desc *grp = (struct ext2_group_desc *)buf;
    printf("%d,", grp->bg_free_blocks_count);
    printf("%d,", grp->bg_free_inodes_count);
    printf("%d,", grp->bg_block_bitmap);
    printf("%d,", grp->bg_inode_bitmap);
    printf("%d\n", grp->bg_inode_table);
		
    // fprintf(stderr, "inode bitmap: %d\n", (int)(grp->bg_inode_bitmap));
		
    // Print free block and inode summary
    int bg_block_bitmap = grp->bg_block_bitmap;
    int bg_inode_bitmap = grp->bg_inode_bitmap;
    free_summary(bg_block_bitmap, blocks_in_group, "BFREE");
    free_summary(bg_inode_bitmap, inodes_in_group, "IFREE");
		
    // Print inode summary
    int bg_inode_table = grp->bg_inode_table;
    inode_summary(bg_inode_table, s_inodes_in_group, s_inode_size);
  }
}
