I made a file system! 

The purpose of this project is to demonstrate simple file system mounting and file and directory creation, modification, and deletion in Linux. 

The file system is called EZFS, and it virtualizes block devices (like disks or drives) using an internal file, hooked up to a loop device.

For commands related to the loop device, see the below terminal output. 

Use the makefile and simple hit 'make' to generate the new file system!

```$ sudo su
# dd if=/dev/zero of=./ext2.img bs=1024 count=224
224+0 records in
224+0 records out
229376 bytes (229 kB, 224 KiB) copied, 0.00198087 s, 116 MB/s
# modprobe loop
# losetup --find --show ext2.img
/dev/loop12
# mkfs -t ext2 /dev/loop12
mke2fs 1.47.0 (1-Jan-2025)
Discarding device blocks: done
Creating filesystem with 56 4k blocks and 32 inodes

Allocating group tables: done
Writing inode tables: done
Writing superblocks and filesystem accounting information: done

# mkdir mnt
# mount /dev/loop12 ./mnt
# df -hT
Filesystem     Type      Size  Used Avail Use% Mounted on
...
/dev/loop12    ext2      200K   24K  168K  13% /home/o_o/mnt
# cd mnt
/mnt# ls -al
total 24
drwxr-xr-x  3 root root  4096 Nov 17 19:39 .
drwxr-x--- 24 o_o  o_o   4096 Nov 17 19:41 ..
drwx------  2 root root 16384 Nov 17 19:39 lost+found
/mnt# mkdir sub2
/mnt# ls -al
total 28
drwxr-xr-x  4 root root  4096 Nov 17 19:42 .
drwxr-x--- 24 o_o  o_o   4096 Nov 17 19:41 ..
drwx------  2 root root 16384 Nov 17 19:39 lost+found
drwxr-xr-x  2 root root  4096 Nov 17 19:42 sub2
/mnt# cd sub2
/mnt/sub2# ls -al
total 8
drwxr-xr-x 2 root root 4096 Nov 17 19:42 .
drwxr-xr-x 4 root root 4096 Nov 17 19:42 ..
/mnt/sub2# mkdir sub2.1
/mnt/sub2# ls -al
total 12
drwxr-xr-x 3 root root 4096 Nov 17 19:42 .
drwxr-xr-x 4 root root 4096 Nov 17 19:42 ..
drwxr-xr-x 2 root root 4096 Nov 17 19:42 sub2.1
/mnt/sub2# touch file2.1
/mnt/sub2# ls -al
total 12
drwxr-xr-x 3 root root 4096 Nov 17 19:42 .
drwxr-xr-x 4 root root 4096 Nov 17 19:42 ..
-rw-r--r-- 1 root root    0 Nov 17 19:42 file2.1
drwxr-xr-x 2 root root 4096 Nov 17 19:42 sub2.1
/mnt/sub2# cd ../../
# umount mnt/
# losetup --find
/dev/loop13
# losetup --detach /dev/loop12
# losetup --find
/dev/loop12
# ls -al mnt/
total 8
drwxr-xr-x  2 root root 4096 Nov 17 19:41 .
drwxr-x--- 24 o_o  o_o  4096 Nov 17 19:41 ..```
