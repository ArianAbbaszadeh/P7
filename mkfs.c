#include "wfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>

int init_filesystem(int disk_img, uint32_t num_inodes, uint32_t num_blocks){
	struct stat st;
	fstat(disk_img, &st);
	uint32_t size_needed = sizeof(struct wfs_sb) +  num_inodes/sizeof(off_t) + num_blocks/sizeof(off_t)
	+ num_inodes * BLOCK_SIZE + num_blocks * BLOCK_SIZE;

	if(st.st_size < size_needed){
		printf("Disk image %ld too small for %d inodes and %d data blocks totaling %d bytes\n", st.st_size, num_inodes, num_blocks, size_needed);
		return 1;
	}

	char* addr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, disk_img, 0);
	close(disk_img);

	struct wfs_sb* sb = (struct wfs_sb*) addr;
	sb->num_inodes = num_inodes;
	sb->num_data_blocks = num_blocks;
	sb->i_bitmap_ptr = (off_t) (sizeof(struct wfs_sb));
	sb->d_bitmap_ptr = (off_t) (sb->i_bitmap_ptr + num_inodes/sizeof(off_t));
	sb->i_blocks_ptr = (off_t) (sb->d_bitmap_ptr + num_blocks/sizeof(off_t));
	sb->d_blocks_ptr = (off_t) (sb->i_blocks_ptr + num_inodes * BLOCK_SIZE);

	char* bitmap = (char*)(addr + sb->i_bitmap_ptr);
	bitmap[0] |= 1;

	struct wfs_inode* root = (struct wfs_inode*)(addr + sb->i_blocks_ptr);
	

	root->gid = getgid();
	root->uid = getuid();
	root->mode = __S_IFDIR;
	time_t curtime = time(NULL);
	root->atim = curtime;
	root->mtim = curtime;
	root->ctim = curtime;
	return 0;
}


int main(int argc, char **argv){
	int opt;
	int disk_img;
	uint32_t num_inodes;
	uint32_t num_data;
	uint32_t temp;
	while((opt = getopt(argc, argv, ":d:i:b:")) != -1){
		switch(opt){
			case 'd':
				if((disk_img = open(optarg, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH | S_IWOTH)) < 0){
					printf("Invalid disk image\n");
					exit(1);
				}
				break;
			case 'i':
				temp = atoi(optarg);
				if(temp == 0){
					printf("Invalid inode count\n");
					exit(1);
				}
				num_inodes = (temp + 31) / 32 * 32;
				break;
			case 'b':
				temp = atoi(optarg);
				if(temp == 0){
					printf("Invalid block count\n");
					exit(1);
				}
				num_data = (temp + 31) / 32 * 32;
				break;
			default:
				printf("USAGE: ./mfks -d <disk image> -i <inode count> -b <block count>");
				exit(1);
		}	
	}
	if(init_filesystem(disk_img, num_inodes, num_data)){
		exit(1);
	}
	return 0;
}