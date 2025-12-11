#ifndef __EZFS_OPS_H__
#define __EZFS_OPS_H__

#include <linux/fs.h>

static struct ezfs_inode *read_ezfs_inode(struct super_block *sb, uint64_t inode_no);
static struct inode *myezfs_get_inode(struct super_block *sb, uint64_t inode_no);

static const struct file_operations myezfs_file_ops;
static const struct inode_operations myezfs_inode_ops;
static const struct address_space_operations myezfs_aops;
static const struct super_operations myezfs_super_ops;

#endif /* ifndef __EZFS_OPS_H__ */
