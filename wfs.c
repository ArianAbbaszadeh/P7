#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include "wfs.h"
#include <fuse.h>


char* addr;
struct wfs_sb* superblock;

static int search_directory(struct wfs_inode* inode, const char* name) {
    printf("searcing inode %d for name: %s\n", inode->num, name);
    if (!S_ISDIR(inode->mode)) {
        printf("Invalid path\n");
        return -1;
    }

    for (int i = 0; i < inode->nlinks; i++) {
        struct wfs_dentry* dentries = (struct wfs_dentry*)(addr + inode->blocks[i]);
        for (int j = 0; j < (BLOCK_SIZE / sizeof(struct wfs_dentry)); j++) {
            struct wfs_dentry tempdentry = dentries[j];
            printf("direntry: %s\n", tempdentry.name);
            if (strcmp(name, tempdentry.name) == 0) {
                // If the name ends with '/', it means we need to search further
                char* next_token = strtok(NULL, "/");
                if (next_token) {
                    struct wfs_inode* next_inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + tempdentry.num * BLOCK_SIZE);
                    return search_directory(next_inode, next_token);
                } else {
                    // If there are no more tokens, we have found the inode
                    return tempdentry.num;
                }
            }
        }
    }
    printf("Path not found\n");
    return -1;
}

int get_inode_from_path(const char* path) {
    printf("Entered search for path: %s\n", path);

    if (strcmp(path, "/") == 0) {
        return 0;
    }
    char* tmp_path = (char*)malloc(sizeof(char) * (strlen(path) + 1));
    strcpy(tmp_path, path);
    tmp_path[27] = '\0';

    struct wfs_inode* root_inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr);
    char* token = strtok(tmp_path, "/");
    int inode_num = search_directory(root_inode, token);

    free(tmp_path);
    return inode_num;
}

off_t alloc_block(){
    printf("allocing block\n");
    int blocknum = -1; 
    char* bitmap = (char*)(addr + superblock->d_bitmap_ptr);
    for(char i = 0; i < superblock->num_inodes; i++){
        int byte_off = i/8;
        int bit_off = 7 - i%8;
        int temp = 1;
        for(int i = 0; i < bit_off; i++){
            temp *= 2;
        }
        if((bitmap[byte_off] & temp) >> bit_off == 0){
            printf("block %d is open at address %lx\n", i, &bitmap[byte_off] - addr);
            blocknum = i;
            bitmap[byte_off] |= temp;
            break;
        }
    }

    return (off_t)(superblock->d_blocks_ptr + blocknum * BLOCK_SIZE);
}

int create_dentry(struct wfs_inode* p_inode, char* name, int inum){
    printf("creating dentry %s for inode %d from parent inode %d", name, inum, p_inode->num);
    printf("n links: %d\n", p_inode->nlinks);
    if(p_inode->nlinks == 0){
        if((p_inode->blocks[0] = alloc_block()) == -1){
            return -1;
        }
        p_inode->size += sizeof(struct wfs_dentry);
        p_inode->nlinks++;
    }
    for(int i = 0; i < p_inode->nlinks; i++){
        printf("checking block %d of inode %d\n", i, p_inode->num);
        struct wfs_dentry* dentries = (struct wfs_dentry*)(addr + p_inode->blocks[i]);
        for(int j = 0; j < (BLOCK_SIZE/sizeof(struct wfs_dentry)); j++){
            if(dentries[j].num == 0){
                memcpy(&dentries[j].num, &inum, sizeof(int));
                memcpy(&dentries[j].name, name, sizeof(dentries[j].name)); 
                p_inode->size += sizeof(struct wfs_dentry);
                time_t curtime = time(NULL);
                p_inode->mtim = curtime;
                p_inode->atim = curtime;
                p_inode->ctim = curtime;
                dentries[j].name[sizeof(dentries[j].name) - 1] = '\0';
                return 0;
            }
        }
    }
    if(p_inode->nlinks < N_BLOCKS){
        if((p_inode->blocks[0] = alloc_block()) == -1){
            return -1;
        }
        p_inode->size += sizeof(struct wfs_dentry);
        p_inode->nlinks++;
    }

    return -1;
}

struct wfs_inode* allocate_inode(){
    printf("allocating inode\n");
    int new_inum = -1;
    char* bitmap = (char*)(addr + superblock->i_bitmap_ptr);
    for(char i = 0; i < superblock->num_inodes; i++){
        int byte_off = i/8;
        int bit_off = 7 - i%8;
        int temp = 1;
        for(int i = 0; i < bit_off; i++){
            temp *= 2;
        }
        if(((bitmap[byte_off] & temp) >> bit_off) == 0){
            printf("inode %d is open at address %lx\n", i, &bitmap[byte_off] - addr);
            new_inum = i;
            bitmap[byte_off] |= temp;
            break;
        }
    }
    if(new_inum == -1){
        return NULL;
    }
    struct wfs_inode* inode = (struct wfs_inode*) (addr + superblock->i_blocks_ptr + new_inum * BLOCK_SIZE);
    inode->num = new_inum;
    return inode;
}

void free_inode(struct wfs_inode* inode){
    int byte_off = inode->num/8;
    int bit_off = inode->num % 8;
    int temp = 1;
    int total = 0;
    for(int i = 0; i < 8; i++){
        if(i == bit_off)
            continue;
        total += temp;
        temp *= 2;
    }
    char* bitmap = (addr + superblock->i_bitmap_ptr + inode->num);
    bitmap[byte_off] &= total;
    memset((struct wfs_inode*)(addr + superblock->i_bitmap_ptr + inode->num * BLOCK_SIZE), 0, sizeof(struct wfs_inode));
}

static int wfs_getattr(const char* path, struct stat* buf){
	printf("Entered getattr for path %s\n", path);
	int inode_num = get_inode_from_path(path);
	if(inode_num < 0){
		printf("invalid inode\n");
		return inode_num;
	}
	struct wfs_inode* inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + inode_num * BLOCK_SIZE);
    printf("inode_num for path %s: %d\n", path, inode_num);
	buf->st_uid = inode->uid;
	buf->st_gid = inode->gid;
	buf->st_atime = time(NULL);
	buf->st_mtime = inode->mtim;
	buf->st_mode = inode->mode;
	buf->st_size = inode->size;
	return 0;
}

static int wfs_mknod(const char* path, mode_t mode, dev_t rdev){
    printf("Entered mknod for path: %s, mode: %d, rdev: %ld\n", path, mode, rdev);
    if(get_inode_from_path(path) != -1){
        printf("file already exists");
        return -EEXIST;
    }

    struct wfs_inode* inode = allocate_inode();
    printf("%d\n", inode->num);
    if(inode == NULL){
        printf("Not enough space");
        return -ENOSPC;
    }
    
    char* parent = dirname(strdup(path));
    
    int parent_inum = get_inode_from_path(parent);
    free(parent);
    if(parent_inum == -1){
        printf("Parent directory not found\n");
        free_inode(inode);
        return -ENOENT;
    }
    struct wfs_inode* parent_inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + parent_inum * BLOCK_SIZE);
    char* tmp_path = strdup(path);
    char* name = basename(tmp_path);
    //printf("basename: %s\n", name);
    if(create_dentry(parent_inode, name, inode->num) == -1){
        printf("No space in parent directory");
        free_inode(inode);
        free(tmp_path);
        return -ENOSPC;
    }
    free(tmp_path);
    printf("%lx\n", ((char*)inode - addr));
    inode->gid = getgid();
    inode->uid = getuid();
    inode->mode = mode;
    time_t curtime = time(NULL);
    inode->atim = curtime;
    inode->mtim = curtime;
    inode->ctim = curtime;

    printf("Created inode with inode number: %d\n", inode->num);
    return 0;
}   

static int wfs_mkdir(const char* path, mode_t mode){
	return (wfs_mknod(path, mode, 0));
}

static int wfs_unlink(const char* path){
    return 0;
}

static int wfs_rmdir(const char* path){
    return 0;
}

static int wfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi){
    printf("Entering read for path %s and size %ld\n", path, size);
    int num_bytes = 0; 
    int inum = get_inode_from_path(path);
    if(inum < 0){
        printf("File %s not found\n", path);
        return -ENOENT;
    }
    struct wfs_inode* inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + inum * BLOCK_SIZE);
    off_t start = offset;
    if(start > inode->size){
        return 0;
    }
    off_t end = offset + size > inode->size ? inode->size : offset + size;
   
    for(int i = start/BLOCK_SIZE; i < N_BLOCKS; i++){
        if(start >= end){
            return num_bytes;
        }
        char* block_ptr = (addr + inode->blocks[i]);
        
        size_t to_copy;

        if(end - start <= (block_ptr + BLOCK_SIZE) - (block_ptr + start % BLOCK_SIZE)){
            to_copy = end - start;
        } else {
            to_copy = (block_ptr + BLOCK_SIZE) - (block_ptr + start% BLOCK_SIZE);
        }
        memcpy(buf + num_bytes, block_ptr + start % BLOCK_SIZE, to_copy);
        num_bytes += to_copy;
        start += num_bytes;
    }

    return num_bytes;
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi){
    return 1;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi){
    return 0;
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod   = wfs_mknod,
    .mkdir   = wfs_mkdir,
    .unlink  = wfs_unlink,
    .rmdir   = wfs_rmdir,
    .read    = wfs_read,
    .write   = wfs_write,
    .readdir = wfs_readdir,
};

int main(int argc, char** argv){
	if(argc < 3){
		printf("USAGE: ./wfs <disk image> -f -s <mount dir>\n");
		exit(1);
	}
	int disk_img;
	if((disk_img = open(argv[1], O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH | S_IWOTH)) < 0){
		printf("invalid disk image");
		exit(1);
	}
	struct stat st;
	fstat(disk_img, &st);
	addr = mmap(0, st.st_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, disk_img, 0);
    superblock = (struct wfs_sb*)addr;
	close(disk_img);
    
    struct stat* buf =(struct stat*) malloc(sizeof(struct stat)); 
    wfs_mknod("/file0", __S_IFDIR, 0);
    wfs_mknod("/dir0/file00", __S_IFREG, 0);

    wfs_getattr("/dir0/file00", buf);
    printf("uid %d, gid: %d, atim: %ld, mtim: %ld,size: %ld, mode: %d\n", buf->st_uid, buf->st_gid, buf->st_atime, buf->st_mtime, buf->st_size, buf->st_mode);
    free(buf);
    return 0;
    
    int fuse_stat = fuse_main(argc - 2, argv + 2, &ops, NULL); 
    return fuse_stat;
}