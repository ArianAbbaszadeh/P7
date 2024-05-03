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
#include <errno.h>
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
    int* bitmap = (int*)(addr + superblock->d_bitmap_ptr);
    for(char i = 0; i < superblock->num_inodes; i++){
        int byte_off = i/32;
        int bit_off = i%32;
        int temp = 1 << bit_off;
        if((bitmap[byte_off] & temp) >> bit_off == 0){
            printf("block %d is open\n", i);
            blocknum = i;
            bitmap[byte_off] |= temp;
            break;
        }
    }
    if(blocknum == -1){
        return -1;
    }
    memset(addr + superblock->d_blocks_ptr + blocknum * BLOCK_SIZE, 0, BLOCK_SIZE);
    return (off_t)(superblock->d_blocks_ptr + blocknum * BLOCK_SIZE);
}

void free_block(off_t block_off){
    int blocknum = (block_off - superblock->d_blocks_ptr) / BLOCK_SIZE;
    int byte_off = blocknum/32;
    int bit_off = blocknum%32;
    int temp = ~(1 << bit_off);

    int* bitmap = (int*)(addr + superblock->d_bitmap_ptr);
    bitmap[byte_off] &= temp;
    memset((addr + superblock->d_blocks_ptr + blocknum * BLOCK_SIZE), 0, BLOCK_SIZE);
}

int create_dentry(struct wfs_inode* p_inode, char* name, int inum){
    printf("creating dentry %s for inode %d from parent inode %d", name, inum, p_inode->num);
    printf("n links: %d\n", p_inode->nlinks);
    for(int i = 0; i < IND_BLOCK; i++){
        if(p_inode->blocks[i] == (off_t)0){
            if((p_inode->blocks[i] = alloc_block()) == -1){
                p_inode->blocks[i] = 0;
                return -1;
            }
            p_inode->nlinks++;
        }
        printf("checking block %d of inode %d\n", i, p_inode->num);
        struct wfs_dentry* dentries = (struct wfs_dentry*)(addr + p_inode->blocks[i]);
        for(int j = 0; j < (BLOCK_SIZE/sizeof(struct wfs_dentry)); j++){
            if(dentries[j].num == 0){
                dentries[j].num = inum;
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
    if(p_inode->nlinks < IND_BLOCK){
        if((p_inode->blocks[0] = alloc_block()) == -1){
            return -1;
        }
        p_inode->size += sizeof(struct wfs_dentry);
        p_inode->nlinks++;
        return create_dentry(p_inode, name, inum);
    }

    return -1;
}

struct wfs_inode* allocate_inode(){
    printf("allocating inode\n");
    int new_inum = -1;
    int* bitmap = (int*)(addr + superblock->i_bitmap_ptr);
    for(int i = 0; i < superblock->num_inodes; i++){
        int byte_off = i/32;
        int bit_off = i%32;
        int temp = 1 << bit_off;
        if(((bitmap[byte_off] & temp) >> bit_off) == 0){
            printf("inode %d is open ", i);
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

void free_inode(struct wfs_inode* p_inode, struct wfs_inode* inode){
    printf("Freeing inode %d with parent inode %d\n", inode->num, p_inode->num);
    int byte_off = inode->num/32;
    int bit_off = inode->num % 32;
    int temp = ~(1 << bit_off);
    int* bitmap = (int*)(addr + superblock->i_bitmap_ptr);
    bitmap[byte_off] &= temp;
    for(int i = 0; i < IND_BLOCK; i++){
        if(p_inode->blocks[i] == (off_t)0){
            continue;
        }
        struct wfs_dentry* dentries = (struct wfs_dentry*)(addr + p_inode->blocks[i]);
        for(int j = 0; j < (BLOCK_SIZE/sizeof(struct wfs_dentry)); j++){
            if(dentries[j].num == inode->num){
                printf("removing dentry for inode %d from parent %d\n", inode->num, p_inode->num);
                memset(dentries + j, 0, sizeof(struct wfs_dentry));
                p_inode->size -= sizeof(struct wfs_dentry);
                break;
            }
        }
    }
    for(int i = 0; i < IND_BLOCK; i++){
        if(inode->blocks[i] != (off_t)0){
            free_block(inode->blocks[i]);
            inode->blocks[i] = 0;
        }
    }
    memset((addr + superblock->i_blocks_ptr + inode->num * BLOCK_SIZE), 0, sizeof(struct wfs_inode));
}

static int wfs_getattr(const char* path, struct stat* buf){
	printf("Entered getattr for path %s\n", path);
	int inode_num = get_inode_from_path(path);
	if(inode_num < 0){
		printf("invalid inode returning %d\n", inode_num);
		return -ENOENT;
	}
	struct wfs_inode* inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + inode_num * BLOCK_SIZE);
    printf("inode_num for path %s: %d\n", path, inode_num);
	buf->st_uid = inode->uid;
	buf->st_gid = inode->gid;
	buf->st_atime = time(NULL);
	buf->st_mtime = inode->mtim;
	buf->st_mode |= inode->mode;
	buf->st_size = inode->size;
	return 0;
}

int wfs_mknod(const char* path, mode_t mode, dev_t rdev){
    printf("Entered mknod for path: %s, mode: %d, rdev: %ld\n", path, mode, rdev);
    if(get_inode_from_path(path) != -1){
        printf("file already exists");
        return EEXIST;
    }
    printf("hi\n");
    struct wfs_inode* inode = allocate_inode();
    if(inode == NULL){
        printf("Not enough space\n");
        return -ENOSPC;
    }
    printf("%d\n", inode->num);
    
    char* parent = dirname(strdup(path));
    
    int parent_inum = get_inode_from_path(parent);
    struct wfs_inode* parent_inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + parent_inum * BLOCK_SIZE);
    free(parent);
    if(parent_inum == -1){
        printf("Parent directory not found\n");
        free_inode(parent_inode, inode);
        return -ENOENT;
    }
    char* tmp_path = strdup(path);
    char* name = basename(tmp_path);

    printf("basename: %s\n", name);
    if(create_dentry(parent_inode, name, inode->num) == -1){
        printf("No space in parent directory");
        free_inode(parent_inode, inode);
        free(tmp_path);
        return -ENOENT;
    }
    free(tmp_path);
    printf("%lx\n", ((char*)inode - addr));
    inode->gid = getgid();
    inode->uid = getuid();
    inode->mode |= mode;
    time_t curtime = time(NULL);
    inode->atim = curtime;
    inode->mtim = curtime;
    inode->ctim = curtime;

    printf("Created inode with inode number: %d\n", inode->num);
    return 0;
}   

static int wfs_mkdir(const char* path, mode_t mode){
	return (wfs_mknod(path, __S_IFDIR, 0));
}

static int wfs_unlink(const char* path){
    printf("Unlinking path %s\n", path);
    int inum = get_inode_from_path(path);
    char* parent = dirname(strdup(path));
    int p_inum = get_inode_from_path(parent);
    free(parent);

    if(inum < 0){
        printf("File %s not found\n", path);
        return -ENOENT;
    }
    struct wfs_inode* inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + inum * BLOCK_SIZE);
    if(S_ISDIR(inode->mode)){
        return -ENOENT;
    }
    struct wfs_inode* p_inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + p_inum * BLOCK_SIZE);
    free_inode(p_inode, inode);
    return 0;


}

static int wfs_rmdir(const char* path){
    printf("Entering rmdir for path %s", path);

    int inum = get_inode_from_path(path);
    char* parent = dirname(strdup(path));
    int p_inum = get_inode_from_path(parent);
    free(parent);
    if(inum < 0){
        printf("File %s not found\n", path);
        return -ENOENT;
    }
    struct wfs_inode* inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + inum * BLOCK_SIZE);
    if(!S_ISDIR(inode->mode)){
        return -ENOENT;
    }
    if(S_ISDIR(inode->mode) && inode->size != 0){
        printf("directory must be empty\n");
        return -ENOTEMPTY;
    }
    struct wfs_inode* p_inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + p_inum * BLOCK_SIZE);

    free_inode(p_inode, inode);
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
   
    for(int i = start/BLOCK_SIZE; i < IND_BLOCK; i++){
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
    printf("Writing %ld bytes to file %s\n", size, path);
    int num_bytes = 0; 
    int inum = get_inode_from_path(path);
    if(inum < 0){
        printf("File %s not found\n", path);
        return -ENOENT;
    }
    struct wfs_inode* inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + inum * BLOCK_SIZE);
    if(size > IND_BLOCK * BLOCK_SIZE - offset){
        printf("Not enough space in file\n");
        return -ENOSPC;
    }
    off_t start = offset;
    off_t end = offset + size > IND_BLOCK * BLOCK_SIZE + BLOCK_SIZE/sizeof(off_t) * BLOCK_SIZE ? IND_BLOCK * BLOCK_SIZE + BLOCK_SIZE/sizeof(off_t*) * BLOCK_SIZE : offset + size;
    printf("%ld, %ld\n", start, end);
    for(int i = start/BLOCK_SIZE; i < IND_BLOCK; i++){
        if(start >= end){
            return num_bytes;
        }
        if(inode->blocks[i] == (off_t)0){
            if((inode->blocks[i] = alloc_block()) == -1){
                return -ENOSPC;
            }
            inode->nlinks++;
        }
        char* block_ptr = addr + inode->blocks[i];
        size_t to_write;

        if(end - start <= (block_ptr + BLOCK_SIZE) - (block_ptr + start % BLOCK_SIZE)){
            to_write = end - start;
        } else {
            to_write = (block_ptr + BLOCK_SIZE) - (block_ptr + start% BLOCK_SIZE);
        }
        memcpy(block_ptr + start % BLOCK_SIZE, buf + num_bytes, to_write);
        num_bytes += to_write;
        start += num_bytes;
        inode->size+= to_write;
    }
    if(start >= end){
        return num_bytes;
    }
    if(inode->blocks[IND_BLOCK] == (off_t)0){
        if((inode->blocks[IND_BLOCK] = alloc_block()) == -1){
            inode->blocks[IND_BLOCK] = (off_t)0;
            return -ENOSPC;
        }
    }
    off_t* indirect_block = (off_t*)(addr + inode->blocks[IND_BLOCK]);
    for(int i = 0; i < BLOCK_SIZE/sizeof(off_t); i++){
        if(start >= end){
            return num_bytes;
        }
        if(indirect_block[i] == (off_t)0){
            if((indirect_block[i] = alloc_block()) == -1){
                indirect_block[i] = 0;
                return -ENOSPC;
            }
            inode->nlinks++;
        }
        char* block_ptr = addr + indirect_block[i];
        size_t to_write;

        if(end - start <= (block_ptr + BLOCK_SIZE) - (block_ptr + start % BLOCK_SIZE)){
            to_write = end - start;
        } else {
            to_write = (block_ptr + BLOCK_SIZE) - (block_ptr + start% BLOCK_SIZE);
        }
        memcpy(block_ptr + start % BLOCK_SIZE, buf + num_bytes, to_write);
        num_bytes += to_write;
        start += num_bytes;
        inode->size += to_write;
    }
    return num_bytes;
}
int readdir_helper(struct wfs_inode* inode, void* buf, fuse_fill_dir_t filler){
    if(!S_ISDIR(inode->mode)){
        printf("Reached file\n");
        return 0;
    }
    printf("Looking through inode %d\n", inode->num);
    for(int i = 0; i < IND_BLOCK; i++){
        if(inode->blocks[i] == (off_t)0){
            continue;
        }
        struct wfs_dentry* dentries = (struct wfs_dentry*)(addr + inode->blocks[i]);
        for(int j = 0; j < BLOCK_SIZE/sizeof(struct wfs_dentry); j++){
            if(dentries[j].num > 0){
                printf("adding %s from inode %d\n", dentries[j].name, dentries[j].num);
                if(filler(buf, dentries[j].name, NULL, (off_t)0) != 0){ 
                    return 0;
                }
                //memcpy(buf, dentries + j, sizeof(struct wfs_dentry));
                struct wfs_inode* next_inode = (struct wfs_inode*) (addr + superblock->i_blocks_ptr + dentries[j].num * BLOCK_SIZE); 
                return readdir_helper(next_inode, (char *)buf + sizeof(struct wfs_dentry), filler);
            }
        }
    }
    return 0;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi){
    printf("Entering readdir for path %s\n", path);
    int inum = get_inode_from_path(path);
    if(inum < 0){
        return -ENOENT;
    }

    struct wfs_inode* inode = (struct wfs_inode*)(addr + superblock->i_blocks_ptr + inum * BLOCK_SIZE);
    if(!S_ISDIR(inode->mode)){
        return -EBADF;
    }

    return(readdir_helper(inode, buf, filler));
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
    /*
    //struct stat* buf =(struct stat*) malloc(sizeof(struct stat)); 
    for(int i = 0; i < 100; i++){
        char tmp[7] = "/file0"; 
        tmp[5] = i + 48;
        wfs_mknod(tmp, __S_IFREG, 0);
    }
    //wfs_unlink("/file");

    return 0;
    */
    int fuse_stat = fuse_main(argc - 2, argv + 2, &ops, NULL); 
    return fuse_stat;
}