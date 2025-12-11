#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/iomap.h>
#include "ezfs.h"
#include "ezfs_ops.h"

// initialize the ezfs_sb_lock
static DEFINE_MUTEX(ezfs_sb_lock);

/// @brief Check if a range of data blocks are free
// is start to start+count all free?
// ret0 if used ret1 if free
static int checkIfFree(struct ezfs_super_block *esb, uint64_t start, uint64_t count)
{
	uint64_t i;
	for (i = 0; i < count; i++) {
		if (start + i >= EZFS_MAX_DATA_BLKS)
			return 0;
		if (!IS_SET(esb->free_data_blocks, start + i)) {
			return 0; // Found a used block
		}
	}
	return 1;
}

/// @brief Mark start to start+count range used by CLEARBUTT
static void clearBITinrange(struct ezfs_super_block *esb, uint64_t start, uint64_t count)
{
	uint64_t i;
	for (i = 0; i < count; i++) {
		CLEARBIT(esb->free_data_blocks, start + i);
	}
}

/// @brief Mark start to start+count range free by SETBIT
static void setBITinrange(struct ezfs_super_block *esb, uint64_t start, uint64_t count)
{
	uint64_t i;
	for (i = 0; i < count; i++) {
		SETBIT(esb->free_data_blocks, start + i);
	}
}

/// @brief Allocate or reallocate data blocks for an inode
/// @param inode
/// @param num_blocks: number of blocks that are to be allocated
/// @return 0 on success, negative error code on failure
// case 0: first allocation when nblocks=0, scan bitmap for contiguois hole of size needed_blocks, mark used, update data_blk_num, nblocks, return 0
// case 1: data fits in the file size nblocks>=needed_blocks ->return 0
// case 2: check neighbor, when free mark used and return 0
// case 3 block 1{file1} in use block2{file2} is in use but file1 needs more blocks, scan disk for nblocks+extra_needed free blocks, update data_blk_num to start of new location, mark new blocks used, mark old blocks free, return 0, mark pages dirty so that kernel writes data to new location
// case 4: no free space available not in the neighbors or elsewhere return -ENOSPC
// condition: when reading old data into RAM, unlock/unmap the superblock page because reading from disk might sleep, back when done and locked check if bitmap changed, if changed return Error
static int ezfs_alloc_or_realloc_blocks(struct inode *inode, uint64_t needed_total_blocks)
{
	struct super_block *sb = inode->i_sb;
	struct ezfs_sb_pages *sbi = sb->s_fs_info;
	struct ezfs_inode *ezi = inode->i_private;
	struct ezfs_super_block *esb;
	uint64_t current_nblocks = ezi->nblocks;
	uint64_t extra_needed = needed_total_blocks - current_nblocks;
	uint64_t next_blk_idx;
	uint64_t new_start_idx = 0;
	int i, found = 0;

	if (current_nblocks >= needed_total_blocks)
		return 0;

	mutex_lock(&ezfs_sb_lock);
	esb = (struct ezfs_super_block *)kmap_local_page(sbi->sb_page);
	if (!esb) {
		mutex_unlock(&ezfs_sb_lock);
		return -EIO;
	}
	if (current_nblocks > 0) {
		uint64_t start_blk_idx = ezi->data_blk_num - EZFS_ROOT_DATABLOCK_NUMBER;
		next_blk_idx = start_blk_idx + current_nblocks;

		if (checkIfFree(esb, next_blk_idx, extra_needed)) {
			clearBITinrange(esb, next_blk_idx, extra_needed);
			ezi->nblocks = needed_total_blocks;
			SetPageDirty(sbi->sb_page);
			kunmap_local(esb);
			mutex_unlock(&ezfs_sb_lock);
			return 0;
		}
	}

	for (i = 0; i <= EZFS_MAX_DATA_BLKS - needed_total_blocks; i++) {
		if (checkIfFree(esb, i, needed_total_blocks)) {
			new_start_idx = i;
			found = 1;
			break;
		}
	}
	if (!found) {

		kunmap_local(esb);
		mutex_unlock(&ezfs_sb_lock);
		return -ENOSPC;
	}

	kunmap_local(esb);
	mutex_unlock(&ezfs_sb_lock);

	for (i = 0; i < current_nblocks; i++) {
		struct page *pg = read_mapping_page(inode->i_mapping, i, NULL);
		if (IS_ERR(pg))
			return PTR_ERR(pg);
		put_page(pg);
	}

	mutex_lock(&ezfs_sb_lock);
	esb = (struct ezfs_super_block *)kmap_local_page(sbi->sb_page);

	// Re-verify space is still free, what if the data changes when we unlocked
	if (!checkIfFree(esb, new_start_idx, needed_total_blocks)) {
		kunmap_local(esb);
		mutex_unlock(&ezfs_sb_lock);
		return -ENOSPC;
	}

	clearBITinrange(esb, new_start_idx, needed_total_blocks);

	if (current_nblocks > 0) {
		uint64_t old_start = ezi->data_blk_num - EZFS_ROOT_DATABLOCK_NUMBER;
		setBITinrange(esb, old_start, current_nblocks);
	}

	ezi->data_blk_num = new_start_idx + EZFS_ROOT_DATABLOCK_NUMBER;
	ezi->nblocks = needed_total_blocks;

	for (i = 0; i < current_nblocks; i++) {
		struct page *pg = find_get_page(inode->i_mapping, i);
		if (pg) {
			set_page_dirty(pg);
			put_page(pg);
		}
	}

	SetPageDirty(sbi->sb_page);
	kunmap_local(esb);
	mutex_unlock(&ezfs_sb_lock);
	return 0;
}
//! ChangeEnd

static uint64_t ezfs_alloc_inode(struct super_block *sb)
{
	struct ezfs_sb_pages *sbi = sb->s_fs_info;
	struct ezfs_super_block *esb;
	uint64_t inode_no = 0;
	int i;

	esb = (struct ezfs_super_block *)kmap_local_page(sbi->sb_page);
	if (!esb)
		return 0;

	for (i = 0; i < EZFS_MAX_INODES; i++) {
		if (IS_SET(esb->free_inodes, i)) {
			CLEARBIT(esb->free_inodes, i);
			inode_no = i + 1; /* inodes are 1-indexed */
			break;
		}
	}

	kunmap_local(esb);

	if (inode_no > 0) {
		SetPageDirty(sbi->sb_page);
	}

	return inode_no;
}

static int ezfs_write_inode_to_disk(struct super_block *sb, uint64_t inode_no,
				     struct ezfs_inode *ezi)
{
	struct ezfs_sb_pages *sbi = sb->s_fs_info;
	struct ezfs_inode *istore;

	if (inode_no == 0 || inode_no > EZFS_MAX_INODES)
		return -EINVAL;

	istore = (struct ezfs_inode *)kmap_local_page(sbi->istore_page);
	if (!istore)
		return -ENOMEM;

	memcpy(&istore[inode_no - 1], ezi, sizeof(struct ezfs_inode));

	kunmap_local(istore);

	SetPageDirty(sbi->istore_page);

	return 0;
}

static int ezfs_add_dentry(struct inode *dir, const char *name, uint64_t inode_no)
{
	struct ezfs_inode *parent_ezi;
	struct super_block *sb = dir->i_sb;
	struct address_space *mapping = sb->s_bdev->bd_mapping;
	struct page *page;
	void *data_block;
	struct ezfs_dir_entry *entry;
	int i;
	int ret = -ENOSPC;

	parent_ezi = (struct ezfs_inode *)dir->i_private;
	if (!parent_ezi)
		return -EINVAL;

	page = read_mapping_page(mapping, parent_ezi->data_blk_num, NULL);
	if (IS_ERR(page))
		return PTR_ERR(page);

	data_block = kmap_local_page(page);
	if (!data_block) {
		put_page(page);
		return -ENOMEM;
	}

	for (i = 0; i < EZFS_MAX_CHILDREN; i++) {
		entry = (struct ezfs_dir_entry *)
			((char *)data_block + i * sizeof(struct ezfs_dir_entry));

		if (!entry->active || entry->inode_no == 0) {
			memset(entry, 0, sizeof(struct ezfs_dir_entry));
			strncpy(entry->filename, name, EZFS_MAX_FILENAME_LENGTH - 1);
			entry->filename[EZFS_MAX_FILENAME_LENGTH - 1] = '\0';
			entry->active = 1;
			entry->inode_no = inode_no;
			ret = 0;
			break;
		}
	}

	kunmap_local(data_block);

	if (ret == 0) {
		SetPageDirty(page);
		dir->i_mtime_sec = dir->i_ctime_sec = ktime_get_real_seconds();
		parent_ezi->i_mtime = parent_ezi->i_ctime = dir->i_mtime_sec;
	}

	put_page(page);
	return ret;
}



static int myezfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct ezfs_inode *ezi;
	struct super_block *sb = inode->i_sb;
	struct ezfs_sb_pages *sbi = sb->s_fs_info;
	struct page *page;
	struct address_space *mapping = sb->s_bdev->bd_mapping;
	struct ezfs_dir_entry *entry;
	void *data_block;

	ezi = (struct ezfs_inode *)inode->i_private;
	if (!ezi)
		return -EINVAL;

	/* Handle . and .. */
	if (ctx->pos == 0) {
		if (!dir_emit_dot(file, ctx))
			return 0;
		ctx->pos++;
	}
	if (ctx->pos == 1) {
		if (!dir_emit_dotdot(file, ctx))
			return 0;
		ctx->pos++;
	}

	/* Read directory entries from data blocks */
	page = read_mapping_page(mapping, ezi->data_blk_num, NULL);
	if (IS_ERR(page))
		return PTR_ERR(page);

	data_block = kmap_local_page(page);
	if (!data_block) {
		put_page(page);
		return -ENOMEM;
	}

	if (ctx->pos >= 2) {
		int index = ctx->pos - 2;
		int i;
		struct ezfs_inode *istore = (struct ezfs_inode *)
		    kmap_local_page(sbi->istore_page);
		for (i = index; i < EZFS_MAX_CHILDREN; i++) {
			entry = (struct ezfs_dir_entry *)((char *)data_block + i * sizeof(struct ezfs_dir_entry));

			if (!entry->active || entry->inode_no == 0) {
				ctx->pos++;
				continue;
			}
			struct ezfs_inode *child_ezi = &istore[entry->inode_no - 1];
			if (!dir_emit(ctx, entry->filename,
				      strnlen(entry->filename, EZFS_MAX_FILENAME_LENGTH),
				      entry->inode_no, S_DT(child_ezi->mode))) {
				kunmap_local(data_block);
				put_page(page);
				return 0;
			}
			ctx->pos++;
		}
	}
	kunmap_local(data_block);
	put_page(page);
	return 0;
}

//  loff_t pos is set by VFS when the read is requested, val is the offset in bytes from the start of the file where the read is requested
//  loff_t length is the number of bytes requested to read, set by the VFS as well
// fill in the iomap structure with the mapping information for the requested range, VFS will handle the rest
// specifically addr bdev type offset length
// !NEW: added logic for write
static int myezfs_iomap_begin(struct inode *inode, loff_t pos, loff_t length,
			      unsigned flags, struct iomap *iomap,
			      struct iomap *srcmap)
{
	struct ezfs_inode *ezi_thisfile = inode->i_private;
	pgoff_t blk_offset;
	uint64_t actualBlockNumber;
	uint8_t right_shift = ilog2(EZFS_BLOCK_SIZE);


	// calculate the block index we are reading
	blk_offset = pos >> 12; // EZFS_BLOCK_SIZE is 4096 = 2^12

	// if write op
	if (flags & IOMAP_WRITE) {
		/* Calculate how many blocks we need to cover the END of this write */
		uint64_t end_byte = pos + length;
		uint64_t needed_blocks = (end_byte + EZFS_BLOCK_SIZE - 1) >> 12;

		/* If file currently has fewer blocks than we need, allocate more */
		if (needed_blocks > ezi_thisfile->nblocks) {
			int ret = ezfs_alloc_or_realloc_blocks(inode, needed_blocks);
			if (ret < 0)
				return ret;
		}
	}

	// if no blocks were allocated i.e a file was just created but a read happens
	if (ezi_thisfile->nblocks == 0)  {
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->length = length;
		return 0;
	}

	// Block Number to be read = Start Block + Block Offset
	actualBlockNumber = ezi_thisfile->data_blk_num + blk_offset;

	// actual address will be just left shifted as the data is contiguous
	iomap->addr = (u64)actualBlockNumber << 12; // EZFS_BLOCK_SIZE is 4096 = 2^12

	/* Calculate the remaining length */
	iomap->length = (ezi_thisfile->nblocks - blk_offset) << 12; // EZFS_BLOCK_SIZE is 4096 = 2^12

	iomap->type = IOMAP_MAPPED;
	iomap->offset = pos;
	iomap->bdev = inode->i_sb->s_bdev;

	return 0;
}

// syntax example:
/**
static int fuse_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			  ssize_t written, unsigned int flags,
			  struct iomap *iomap)
*/
// choose to use this function to update simple
// metadata fields like the number of blocks associated with a file.
static int myezfs_iomap_end(struct inode *inode, loff_t pos, loff_t length,
			    ssize_t written, unsigned flags, struct iomap *iomap)
{
	struct ezfs_inode *ezi = inode->i_private;

	if (written > 0) {
		if (pos + written > inode->i_size) {
			i_size_write(inode, pos + written); // Update VFS size
			ezi->file_size = inode->i_size;	    // Update EZFS size
			mark_inode_dirty(inode);
		}
	}
	return 0;
}

static const struct iomap_ops myezfs_iomap_ops = {
    .iomap_begin = myezfs_iomap_begin,
    .iomap_end = myezfs_iomap_end,
};

/// @brief write_iter implementation for myezfs
/// @param iocb
/// @param from
/// @return
/// check if the position is inside the inode size
/// if append flag is set, set the position to the end of the file
/// update the access, modify and change times of the inode, and mark the inode dirty for disk update
/// then call iomap_file_buffered_write
/// if write was successful, call generic_write_sync to ensure data is on disk (this is the )
static ssize_t myezfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	ssize_t ret;
	loff_t pos = iocb->ki_pos;

	if (pos < 0)
		return -EINVAL;

	// on an append write operation write from the end of the file
	if (iocb->ki_flags & IOCB_APPEND)
		iocb->ki_pos = i_size_read(inode); // will be used in iomap_begin when iomap_file_buffered_write is called

	struct timespec64 now = current_time(inode);
	inode->i_atime_sec = inode->i_mtime_sec = inode->i_ctime_sec = now.tv_sec;
	mark_inode_dirty(inode); // to update the change in times we made to inode to the dosk

	ret = iomap_file_buffered_write(iocb, from, &myezfs_iomap_ops, NULL);

	if (ret > 0) // on successful iomap_file_buffered_write
		generic_write_sync(iocb, ret);

	return ret;
}

static const struct file_operations myezfs_file_ops = {
    .llseek = generic_file_llseek,
    .read_iter = generic_file_read_iter,
    .iterate_shared = myezfs_readdir,
    .write_iter = myezfs_file_write_iter,
    .fsync = generic_file_fsync,
};

static struct dentry *myezfs_lookup(struct inode *dir, struct dentry *dentry,
				    unsigned int flags)
{
	struct inode *inode = NULL;
	struct ezfs_inode *parent_ezi;
	struct ezfs_dir_entry *entry;
	struct super_block *sb = dir->i_sb;
	struct ezfs_sb_pages *sbi = dir->i_sb->s_fs_info;
	struct address_space *mapping = dir->i_sb->s_bdev->bd_mapping;
	struct page *page;
	void *data_block;

	if (!dir || !dentry || !sb || !sbi)
		return ERR_PTR(-EINVAL);

	parent_ezi = (struct ezfs_inode *)dir->i_private;
	if (!parent_ezi)
		return ERR_PTR(-EINVAL);

	page = read_mapping_page(mapping, parent_ezi->data_blk_num, NULL);
	if (IS_ERR(page))
		return ERR_CAST(page);

	data_block = kmap_local_page(page);
	if (!data_block) {
		put_page(page);
		return ERR_PTR(-ENOMEM);
	}

	for (int i = 0; i < EZFS_MAX_CHILDREN; i++) {
		entry = (struct ezfs_dir_entry *)((char *)data_block + i * sizeof(struct ezfs_dir_entry));
		if (!entry->active || entry->inode_no == 0)
			continue;
		if (strncmp(dentry->d_name.name, entry->filename, EZFS_MAX_FILENAME_LENGTH) == 0) {
			inode = myezfs_get_inode(sb, entry->inode_no);
			break;
		}
	}
	kunmap_local(data_block);
	put_page(page);

	if (!inode) {
		return d_splice_alias(NULL, dentry);  // Changed from: return ERR_PTR(-ENOENT);
	}

	return d_splice_alias(inode, dentry);
}

static int myezfs_read_folio(struct file *file, struct folio *folio)
{
	return iomap_read_folio(folio, &myezfs_iomap_ops);
}

static const struct address_space_operations myezfs_aops = {
    .read_folio = myezfs_read_folio,
    //     .write_begin = iomap_write_begin,
    //     .write_end = iomap_write_end,
};

static int myezfs_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct ezfs_inode *new_ezi;
	struct ezfs_inode *parent_ezi;
	uint64_t inode_no;
	int ret;
	ktime_t current_time;

	inode_no = ezfs_alloc_inode(sb);
	if (inode_no == 0) {
		return -ENOSPC;
	}

	new_ezi = kzalloc(sizeof(struct ezfs_inode), GFP_KERNEL);
	if (!new_ezi) {
		return -ENOMEM;
	}

	current_time = ktime_get_real_seconds();
	new_ezi->mode = S_IFREG | (mode & 0777);
	new_ezi->uid = from_kuid(&init_user_ns, current_fsuid());
	new_ezi->gid = from_kgid(&init_user_ns, current_fsgid());
	new_ezi->nlink = 1;
	new_ezi->file_size = 0;
	new_ezi->nblocks = 0;
	new_ezi->data_blk_num = 0;
	new_ezi->i_atime = new_ezi->i_mtime = new_ezi->i_ctime = current_time;

	ret = ezfs_write_inode_to_disk(sb, inode_no, new_ezi);
	if (ret) {
		kfree(new_ezi);
		return ret;
	}

	ret = ezfs_add_dentry(dir, dentry->d_name.name, inode_no);
	if (ret) {
		kfree(new_ezi);
		return ret;
	}

	parent_ezi = (struct ezfs_inode *)dir->i_private;
	if (parent_ezi) {
		ezfs_write_inode_to_disk(sb, dir->i_ino, parent_ezi);
	}

	inode = new_inode(sb);
	if (!inode) {
		kfree(new_ezi);
		return -ENOMEM;
	}

	inode->i_ino = inode_no;
	inode->i_private = new_ezi;
	inode->i_mode = S_IFREG | (mode & 0777);
	inode->i_size = 0;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_blocks = 0;
	inode->i_atime_sec = inode->i_mtime_sec = inode->i_ctime_sec = current_time;
	inode->i_sb = sb;
	inode->i_op = &myezfs_inode_ops;
	inode->i_fop = &myezfs_file_ops;
	inode->i_mapping->a_ops = &myezfs_aops;
	set_nlink(inode, 1);
	insert_inode_hash(inode);

	d_instantiate(dentry, inode);

	return 0;
}


static const struct inode_operations myezfs_inode_ops = {
    .lookup = myezfs_lookup,
    .create = myezfs_create,
    // .mkdir = myezfs_mkdir,
    // .rmdir = myezfs_rmdir,
    // .unlink = myezfs_unlink,
    // .rename = myezfs_rename,
};

static struct ezfs_inode *read_ezfs_inode(struct super_block *sb, uint64_t inode_no)
{
	struct page *ipage;
	struct ezfs_inode *result_inode;
	void *idata;

	ipage = read_mapping_page(sb->s_bdev->bd_mapping,
				  EZFS_INODE_STORE_DATABLOCK_NUMBER, NULL);
	if (IS_ERR(ipage))
		return NULL;
	idata = kmap_local_page(ipage);
	if (!idata) {
		put_page(ipage);
		return NULL;
	}
	if (inode_no == 0 || inode_no > EZFS_MAX_INODES) {
		kunmap_local(idata);
		put_page(ipage);
		return NULL;
	}
	result_inode = kzalloc(sizeof(struct ezfs_inode), GFP_KERNEL);
	if (!result_inode) {
		kunmap_local(idata);
		put_page(ipage);
		return NULL;
	}
	memcpy(result_inode,
	       (struct ezfs_inode *)((char *)idata + (inode_no - 1) * sizeof(struct ezfs_inode)),
	       sizeof(struct ezfs_inode));
	kunmap_local(idata);
	put_page(ipage);
	return result_inode;
}

static struct inode *myezfs_get_inode(struct super_block *sb, uint64_t inode_no)
{
	struct inode *inode;
	struct ezfs_inode *new_ezi;

	if (!sb)
		return NULL;

	new_ezi = read_ezfs_inode(sb, inode_no);
	if (!new_ezi)
		return NULL;

	inode = iget_locked(sb, inode_no);
	if (!inode) {
		kfree(new_ezi);
		return NULL;
	}
	if (inode->i_state & I_NEW) {
		inode->i_private = new_ezi;
		inode->i_mode = new_ezi->mode;
		inode->i_size = new_ezi->file_size;
		inode->i_uid = KUIDT_INIT(new_ezi->uid);
		inode->i_gid = KGIDT_INIT(new_ezi->gid);
		inode->i_blocks = new_ezi->nblocks * (EZFS_BLOCK_SIZE / 512);
		inode->i_atime_sec = new_ezi->i_atime;
		inode->i_mtime_sec = new_ezi->i_mtime;
		inode->i_ctime_sec = new_ezi->i_ctime;
		inode->i_sb = sb;
		inode->i_op = &myezfs_inode_ops;
		inode->i_fop = &myezfs_file_ops;
		inode->i_mapping->a_ops = &myezfs_aops; // link to the address space operations
		set_nlink(inode, new_ezi->nlink);
		unlock_new_inode(inode);
	} else {
		if (!inode->i_private) {
			inode->i_private = new_ezi;
		} else {
			kfree(new_ezi);
		}
	}
	return inode;
}

// on unmount, put the pages back to the os and free the memory that was allocated with kzalloc on mount/fill_super
static void myezfs_kill_sb(struct super_block *sb)
{
	struct ezfs_sb_pages *sbi = sb->s_fs_info;

	if (sbi) {
		// check if the sb_page is valid before putting it back
		if (sbi->sb_page)
			put_page(sbi->sb_page);
		// check if the istore_page is valid before putting it back
		if (sbi->istore_page)
			put_page(sbi->istore_page);
		kfree(sbi);
		kill_block_super(sb);
	}

	/* if we allocated persistent copies for inodes, free them */
	if (sb->s_root) {
		struct inode *root = sb->s_root->d_inode;
		if (root && root->i_private) {
			kfree(root->i_private);
			root->i_private = NULL;
		}
	}
}

static int myezfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ezfs_sb_pages *sbi;
	struct page *sb_page, *istore_page;
	struct address_space *mapping = sb->s_bdev->bd_mapping;
	struct inode *root_inode;
	struct ezfs_super_block *esb;
	struct ezfs_inode *root_ezfs_inode, *temp_inode;

	sb_set_blocksize(sb, EZFS_BLOCK_SIZE);

	sbi = kzalloc(sizeof(struct ezfs_sb_pages), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	sb_page = read_mapping_page(mapping, EZFS_SUPERBLOCK_DATABLOCK_NUMBER, NULL);

	if (IS_ERR(sb_page))
		return PTR_ERR(sb_page);
	sbi->sb_page = sb_page; // save superblock page
	esb = (struct ezfs_super_block *)kmap_local_page(sb_page);
	if (esb->magic != EZFS_MAGIC_NUMBER) {
		kunmap_local(esb);
		return -EINVAL;
	}
	kunmap_local(esb); // unmap the page after reading

	istore_page = read_mapping_page(mapping, EZFS_INODE_STORE_DATABLOCK_NUMBER, NULL);

	if (IS_ERR(istore_page))
		return PTR_ERR(istore_page);
	sbi->istore_page = istore_page; // Save inode page

	temp_inode = (struct ezfs_inode *)kmap_local_page(istore_page);
	if (!temp_inode) {
		return -ENOMEM;
	}

	root_ezfs_inode = kzalloc(sizeof(struct ezfs_inode), GFP_KERNEL);
	if (!root_ezfs_inode) {
		kunmap_local(temp_inode);
		return -ENOMEM;
	}

	*root_ezfs_inode = temp_inode[EZFS_ROOT_INODE_NUMBER - 1];

	kunmap_local(temp_inode);

	root_inode = iget_locked(sb, EZFS_ROOT_INODE_NUMBER); // get root inode
	if (!root_inode) {
		return -ENOMEM;
	}

	root_inode->i_mode = S_IFDIR | 0777;		 // set permissions for inode
	root_inode->i_op = &myezfs_inode_ops;		 // set inode operations
	root_inode->i_fop = &myezfs_file_ops;		 // set file operations
	root_inode->i_private = root_ezfs_inode;	 // link to ezfs inode
	root_inode->i_size = root_ezfs_inode->file_size; // set size
	root_inode->i_mapping->a_ops = &myezfs_aops;	 // set address space operations

	/* TODO - differentiate between dir and file */

	unlock_new_inode(root_inode); // unlock routine req for iget_locked
	sb->s_root = d_make_root(root_inode); // just like bfs
	if (!sb->s_root) {
		return -ENOMEM;
	}
	sb->s_magic = EZFS_MAGIC_NUMBER;

	return 0;
}

static int myezfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, myezfs_fill_super);
}

static const struct fs_context_operations myezfs_fs_context_ops = {
    .get_tree = myezfs_get_tree,
};

static int myezfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &myezfs_fs_context_ops;
	return 0;
}

static struct file_system_type myezfs_fs_type = {
    .name = "myezfs",
    .init_fs_context = myezfs_init_fs_context,
    .kill_sb = myezfs_kill_sb,
    .fs_flags = FS_USERNS_MOUNT | FS_REQUIRES_DEV,
};

static int __init
myezfs_init(void)
{
	return register_filesystem(&myezfs_fs_type);
}
static void __exit myezfs_exit(void)
{
	unregister_filesystem(&myezfs_fs_type);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sol");
MODULE_DESCRIPTION("ezfs filesystem for CS4118 Homework 6");
MODULE_ALIAS_FS("myezfs");
module_init(myezfs_init);
module_exit(myezfs_exit);