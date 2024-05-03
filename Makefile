BINS = clean wfs mkfs
CC = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18 -g
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`
.PHONY: all

test: all
	./create_disk.sh 
	./mkfs -d disk.img -i 96 -b 64
	./wfs disk.img -s -f mnt
run: all
	./create_disk.sh 
	./mkfs -d disk.img -i 96 -b 200
	umount.sh mnt
	rmdir mnt
	mkdir mnt
	./wfs disk.img -s -f -d mnt

all: $(BINS)
wfs:
	$(CC) $(CFLAGS) wfs.c $(FUSE_CFLAGS) -o wfs
mkfs:
	$(CC) $(CFLAGS) -o mkfs mkfs.c



.PHONY: clean
clean:
	rm -rf $(BINS)
