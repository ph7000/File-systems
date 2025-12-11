#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* These are the same on a 64-bit architecture */
#define timespec64 timespec

#include "ezfs.h"

void passert(int condition, char *message)
{
	printf("[%s] %s\n", condition ? " OK " : "FAIL", message);
	if (!condition)
		exit(1);
}

void inode_reset(struct ezfs_inode *inode)
{
	struct timespec current_time;

	/* In case inode is uninitialized/previously used */
	memset(inode, 0, sizeof(*inode));
	memset(&current_time, 0, sizeof(current_time));

	/* These sample files will be owned by the first user and group on the system */
	inode->uid = 1000;
	inode->gid = 1000;

	/* Current time UTC */
	clock_gettime(CLOCK_REALTIME, &current_time);
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time.tv_sec;
}

void dentry_reset(struct ezfs_dir_entry *dentry)
{
	memset(dentry, 0, sizeof(*dentry));
}

// copy from
void copy_file_to_block(int fd_dest, int block_num, const char *path)
{
	int fd_src = open(path, O_RDONLY);
	if (fd_src < 0) {
		perror("Open src failed");
		exit(1);
	}

	// Jump TO BLOCK NUMBER
	if (lseek(fd_dest, block_num * EZFS_BLOCK_SIZE, SEEK_SET) < 0) {
		perror("Lseek failed");
		exit(1);
	}

	char buf[4096];
	ssize_t bytes;
	// read from source file desc into buff, read 4096 bytes at a time(1 block for ezFS)
	while ((bytes = read(fd_src, buf, sizeof(buf))) > 0) {
		write(fd_dest, buf, bytes);
	}
	close(fd_src);
}

int main(int argc, char *argv[])
{
	int fd;
	int disk_blks;
	ssize_t ret, len;
	struct ezfs_super_block sb;
	struct ezfs_inode inode;
	struct ezfs_dir_entry dentry;

	char *hello_contents = "Hello world!\n";

	char *names_contents = "ShlokDesai\nOpalinaKhanna\nPaulHeyden\n";

	char buf[EZFS_BLOCK_SIZE];
	const char zeroes[EZFS_BLOCK_SIZE] = {0};

	if (argc != 3) {
		printf("Usage: ./format_disk_as_ezfs DEVICE_NAME DISK_BLKS.\n");
		return -1;
	}
	disk_blks = atoi(argv[2]);
	if (disk_blks <= 0) {
		printf("Invalid DISK_BLKS.\n");
		return -1;
	}

	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		perror("Error opening the device");
		return -1;
	}
	memset(&sb, 0, sizeof(sb));

	sb.version = 1;
	sb.magic = EZFS_MAGIC_NUMBER;
	sb.disk_blks = disk_blks;

	// set inode bits in sb
	for (int i = 0; i <= 6; i++)
		SETBIT(sb.free_inodes, i);

	for (int i = 0; i <= 15; i++)
		SETBIT(sb.free_data_blocks, i);

	/* Write the superblock to the first block of the filesystem. */
	ret = write(fd, (char *)&sb, sizeof(sb));
	passert(ret == EZFS_BLOCK_SIZE, "Write superblock");

	inode_reset(&inode);
	inode.mode = S_IFDIR | 0777;
	inode.nlink = 3; // Self, parent and subdir
	inode.data_blk_num = EZFS_ROOT_DATABLOCK_NUMBER;
	inode.file_size = EZFS_BLOCK_SIZE;
	inode.nblocks = 1;

	/* Write the root inode starting in the second block. */
	ret = write(fd, (char *)&inode, sizeof(inode));
	passert(ret == sizeof(inode), "Write root inode");

	inode_reset(&inode);
	inode.nlink = 1;
	inode.mode = S_IFREG | 0666;
	inode.data_blk_num = EZFS_ROOT_DATABLOCK_NUMBER + 1; // Block 3
	inode.file_size = strlen(hello_contents);
	inode.nblocks = 1;

	ret = write(fd, (char *)&inode, sizeof(inode));
	passert(ret == sizeof(inode), "Write hello.txt inode");

	// subdir inode
	inode_reset(&inode);
	inode.mode = S_IFDIR | 0777;
	inode.nlink = 2;	// self and parent
	inode.data_blk_num = 4; //
	inode.file_size = EZFS_BLOCK_SIZE;
	inode.nblocks = 1;
	write(fd, (char *)&inode, sizeof(inode));

	// names Inode
	inode_reset(&inode);
	inode.mode = S_IFREG | 0666;
	inode.nlink = 1;
	inode.data_blk_num = 5;
	inode.file_size = strlen(names_contents);
	inode.nblocks = 1;
	write(fd, (char *)&inode, sizeof(inode));

	// img Inode
	inode_reset(&inode);
	inode.mode = S_IFREG | 0666;
	inode.nlink = 1;
	inode.data_blk_num = 6;

	struct stat st;
	uint64_t file_size;
	if (stat("big_files/big_img.jpeg", &st) == 0) {
		file_size = st.st_size;
		memset(&st, 0, sizeof(st));
	} else {
		perror("stat failed");
		exit(1);
	}
	inode.file_size = file_size;
	inode.nblocks = 8;
	write(fd, (char *)&inode, sizeof(inode));

	// bigtxt Inode
	inode_reset(&inode);
	inode.mode = S_IFREG | 0666;
	inode.nlink = 1;
	inode.data_blk_num = 14;
	if (stat("big_files/big_txt.txt", &st) == 0) {
		file_size = st.st_size;
		memset(&st, 0, sizeof(st));
	} else {
		perror("stat failed");
		exit(1);
	}
	inode.file_size = file_size;
	inode.nblocks = 2;
	write(fd, (char *)&inode, sizeof(inode));

	// seek to block 2
	ret = lseek(fd, EZFS_ROOT_DATABLOCK_NUMBER * EZFS_BLOCK_SIZE, SEEK_SET);
	passert(ret >= 0, "Seek past inode table");

	// dentry for hello
	dentry_reset(&dentry);
	strncpy(dentry.filename, "hello.txt", sizeof(dentry.filename));
	dentry.active = 1;
	dentry.inode_no = EZFS_ROOT_INODE_NUMBER + 1; // Inode 2

	ret = write(fd, (char *)&dentry, sizeof(dentry));
	passert(ret == sizeof(dentry), "Write dentry for hello.txt");

	// dentry for subdir
	dentry_reset(&dentry);
	strncpy(dentry.filename, "subdir", sizeof(dentry.filename));
	dentry.active = 1;
	dentry.inode_no = 3; // Inode 3
	write(fd, (char *)&dentry, sizeof(dentry));

	len = EZFS_BLOCK_SIZE - (2 * sizeof(struct ezfs_dir_entry));

	ret = write(fd, zeroes, len);
	passert(ret == len, "Pad to end of root dentries");

	// data for hello.txt
	len = strlen(hello_contents);
	strncpy(buf, hello_contents, len);
	ret = write(fd, buf, len);
	passert(ret == len, "Write hello.txt contents");

	// data for subdir, containing dentries for names.txt, big_img.jpeg, big_txt.txt
	lseek(fd, 4 * EZFS_BLOCK_SIZE, SEEK_SET);

	// names.txt
	dentry_reset(&dentry);
	strncpy(dentry.filename, "names.txt", sizeof(dentry.filename));
	dentry.active = 1;
	dentry.inode_no = 4;
	ret = write(fd, &dentry, sizeof(dentry));
	passert(ret == sizeof(dentry), "Write dentry for names.txt");

	// bigimage.jpeg
	dentry_reset(&dentry);
	strncpy(dentry.filename, "big_img.jpeg", sizeof(dentry.filename));
	dentry.active = 1;
	dentry.inode_no = 5;
	ret = write(fd, &dentry, sizeof(dentry));
	passert(ret == sizeof(dentry), "Write dentry for big_img.jpeg");

	// bigtext.txt
	dentry_reset(&dentry);
	strncpy(dentry.filename, "big_txt.txt", sizeof(dentry.filename));
	dentry.active = 1;
	dentry.inode_no = 6;
	ret = write(fd, &dentry, sizeof(dentry));
	passert(ret == sizeof(dentry), "Write dentry for big_txt.txt");

	// Pad remaining datablock of dentries with zeroes
	len = EZFS_BLOCK_SIZE - (3 * sizeof(struct ezfs_dir_entry));
	write(fd, zeroes, len);

	// names file
	lseek(fd, 5 * EZFS_BLOCK_SIZE, SEEK_SET);
	write(fd, names_contents, strlen(names_contents));

	copy_file_to_block(fd, 6, "big_files/big_img.jpeg");

	// big text file
	copy_file_to_block(fd, 14, "big_files/big_txt.txt");

	ret = fsync(fd);
	passert(ret == 0, "Flush writes to disk");

	close(fd);
	printf("Device [%s] formatted successfully.\n", argv[1]);

	return 0;
}
