/*
 *  linux/fs/ext3cow/namei.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *	  David S. Miller (davem@caip.rutgers.edu), 1995
 *  Directory entry file type support and forward compatibility hooks
 *	for B-tree directories by Theodore Ts'o (tytso@mit.edu), 1998
 *  Hash Tree Directory indexing (c)
 *	Daniel Phillips, 2001
 *  Hash Tree Directory indexing porting
 *	Christopher Li, 2002
 *  Hash Tree Directory indexing cleanup
 *	Theodore Ts'o, 2002
 */

#include <linux/quotaops.h>
#include "ext3cow.h"
#include "namei.h"
#include "xattr.h"
#include "acl.h"

/*
 * define how far ahead to read directories while searching them.
 */
#define NAMEI_RA_CHUNKS  2
#define NAMEI_RA_BLOCKS  4
#define NAMEI_RA_SIZE	     (NAMEI_RA_CHUNKS * NAMEI_RA_BLOCKS)
#define NAMEI_RA_INDEX(c,b)  (((c) * NAMEI_RA_BLOCKS) + (b))

/* is the inode marked unchangeable or does the name
    contain an epoch less than the current system epoch -znjp */
int is_unchangeable(struct inode *inode, struct dentry *dentry){
      
    char *at = NULL;
    if (inode && (EXT3COW_IS_UNCHANGEABLE(inode) || IS_IMMUTABLE(inode)))
	return 1;
    if(dentry)
	at = strrchr(dentry->d_name.name, EXT3COW_FLUX_TOKEN);
    if(at && (simple_strtol(&at[1], (char **)NULL, 10) > 0))
	return 1;
    return 0;
}


static struct buffer_head *ext3cow_append(handle_t *handle,
					struct inode *inode,
					u32 *block, int *err)
{
	struct buffer_head *bh;

	*block = inode->i_size >> inode->i_sb->s_blocksize_bits;

	bh = ext3cow_bread(handle, inode, *block, 1, err);
	if (bh) {
		inode->i_size += inode->i_sb->s_blocksize;
		EXT3COW_I(inode)->i_disksize = inode->i_size;
		*err = ext3cow_journal_get_write_access(handle, bh);
		if (*err) {
			brelse(bh);
			bh = NULL;
		}
	}
	return bh;
}

#ifndef assert
#define assert(test) J_ASSERT(test)
#endif

#ifdef DX_DEBUG
#define dxtrace(command) command
#else
#define dxtrace(command)
#endif

struct fake_dirent
{
	__le32 inode;
	__le16 rec_len;
	u8 name_len;
	u8 file_type;
};

struct dx_countlimit
{
	__le16 limit;
	__le16 count;
};

struct dx_entry
{
	__le32 hash;
	__le32 block;
};

/*
 * dx_root_info is laid out so that if it should somehow get overlaid by a
 * dirent the two low bits of the hash version will be zero.  Therefore, the
 * hash version mod 4 should never be 0.  Sincerely, the paranoia department.
 */

struct dx_root
{
	struct fake_dirent dot;
	char dot_name[4];
	struct fake_dirent dotdot;
	char dotdot_name[4];
	struct dx_root_info
	{
		__le32 reserved_zero;
		u8 hash_version;
		u8 info_length; /* 8 */
		u8 indirect_levels;
		u8 unused_flags;
	}
	info;
	struct dx_entry	entries[0];
};

struct dx_node
{
	struct fake_dirent fake;
	struct dx_entry	entries[0];
};


struct dx_frame
{
	struct buffer_head *bh;
	struct dx_entry *entries;
	struct dx_entry *at;
};

struct dx_map_entry
{
	u32 hash;
	u16 offs;
	u16 size;
};

static inline unsigned dx_get_block (struct dx_entry *entry);
static void dx_set_block (struct dx_entry *entry, unsigned value);
static inline unsigned dx_get_hash (struct dx_entry *entry);
static void dx_set_hash (struct dx_entry *entry, unsigned value);
static unsigned dx_get_count (struct dx_entry *entries);
static unsigned dx_get_limit (struct dx_entry *entries);
static void dx_set_count (struct dx_entry *entries, unsigned value);
static void dx_set_limit (struct dx_entry *entries, unsigned value);
static unsigned dx_root_limit (struct inode *dir, unsigned infosize);
static unsigned dx_node_limit (struct inode *dir);
static struct dx_frame *dx_probe(struct qstr *entry,
				 struct inode *dir,
				 struct dx_hash_info *hinfo,
				 struct dx_frame *frame,
				 int *err);
static void dx_release (struct dx_frame *frames);
static int dx_make_map(struct ext3cow_dir_entry_2 *de, unsigned blocksize,
			struct dx_hash_info *hinfo, struct dx_map_entry map[]);
static void dx_sort_map(struct dx_map_entry *map, unsigned count);
static struct ext3cow_dir_entry_2 *dx_move_dirents (char *from, char *to,
		struct dx_map_entry *offsets, int count);
static struct ext3cow_dir_entry_2 *dx_pack_dirents(char *base, unsigned blocksize);
static void dx_insert_block (struct dx_frame *frame, u32 hash, u32 block);
static int ext3cow_htree_next_block(struct inode *dir, __u32 hash,
				 struct dx_frame *frame,
				 struct dx_frame *frames,
				 __u32 *start_hash);
static struct buffer_head * ext3cow_dx_find_entry(struct inode *dir,
			struct qstr *entry, struct ext3cow_dir_entry_2 **res_dir,
			int *err);
static int ext3cow_dx_add_entry(handle_t *handle, struct dentry *dentry,
			     struct inode *inode);

/*
 * p is at least 6 bytes before the end of page
 */
static inline struct ext3cow_dir_entry_2 *
ext3cow_next_entry(struct ext3cow_dir_entry_2 *p)
{
	return (struct ext3cow_dir_entry_2 *)((char *)p +
		ext3cow_rec_len_from_disk(p->rec_len));
}

/*
 * Future: use high four bits of block for coalesce-on-delete flags
 * Mask them off for now.
 */

static inline unsigned dx_get_block (struct dx_entry *entry)
{
	return le32_to_cpu(entry->block) & 0x00ffffff;
}

static inline void dx_set_block (struct dx_entry *entry, unsigned value)
{
	entry->block = cpu_to_le32(value);
}

static inline unsigned dx_get_hash (struct dx_entry *entry)
{
	return le32_to_cpu(entry->hash);
}

static inline void dx_set_hash (struct dx_entry *entry, unsigned value)
{
	entry->hash = cpu_to_le32(value);
}

static inline unsigned dx_get_count (struct dx_entry *entries)
{
	return le16_to_cpu(((struct dx_countlimit *) entries)->count);
}

static inline unsigned dx_get_limit (struct dx_entry *entries)
{
	return le16_to_cpu(((struct dx_countlimit *) entries)->limit);
}

static inline void dx_set_count (struct dx_entry *entries, unsigned value)
{
	((struct dx_countlimit *) entries)->count = cpu_to_le16(value);
}

static inline void dx_set_limit (struct dx_entry *entries, unsigned value)
{
	((struct dx_countlimit *) entries)->limit = cpu_to_le16(value);
}

static inline unsigned dx_root_limit (struct inode *dir, unsigned infosize)
{
	unsigned entry_space = dir->i_sb->s_blocksize - EXT3COW_DIR_REC_LEN(1) -
		EXT3COW_DIR_REC_LEN(2) - infosize;
	return entry_space / sizeof(struct dx_entry);
}

static inline unsigned dx_node_limit (struct inode *dir)
{
	unsigned entry_space = dir->i_sb->s_blocksize - EXT3COW_DIR_REC_LEN(0);
	return entry_space / sizeof(struct dx_entry);
}

/*
 * Debug
 */
#ifdef DX_DEBUG
static void dx_show_index (char * label, struct dx_entry *entries)
{
	int i, n = dx_get_count (entries);
	printk("%s index ", label);
	for (i = 0; i < n; i++)
	{
		printk("%x->%u ", i? dx_get_hash(entries + i): 0, dx_get_block(entries + i));
	}
	printk("\n");
}

struct stats
{
	unsigned names;
	unsigned space;
	unsigned bcount;
};

static struct stats dx_show_leaf(struct dx_hash_info *hinfo, struct ext3cow_dir_entry_2 *de,
				 int size, int show_names)
{
	unsigned names = 0, space = 0;
	char *base = (char *) de;
	struct dx_hash_info h = *hinfo;

	printk("names: ");
	while ((char *) de < base + size)
	{
		if (de->inode)
		{
			if (show_names)
			{
				int len = de->name_len;
				char *name = de->name;
				while (len--) printk("%c", *name++);
				ext3cowfs_dirhash(de->name, de->name_len, &h);
				printk(":%x.%u ", h.hash,
				       (unsigned) ((char *) de - base));
			}
			space += EXT3COW_DIR_REC_LEN(de->name_len);
			names++;
		}
		de = ext3cow_next_entry(de);
	}
	printk("(%i)\n", names);
	return (struct stats) { names, space, 1 };
}

struct stats dx_show_entries(struct dx_hash_info *hinfo, struct inode *dir,
			     struct dx_entry *entries, int levels)
{
	unsigned blocksize = dir->i_sb->s_blocksize;
	unsigned count = dx_get_count (entries), names = 0, space = 0, i;
	unsigned bcount = 0;
	struct buffer_head *bh;
	int err;
	printk("%i indexed blocks...\n", count);
	for (i = 0; i < count; i++, entries++)
	{
		u32 block = dx_get_block(entries), hash = i? dx_get_hash(entries): 0;
		u32 range = i < count - 1? (dx_get_hash(entries + 1) - hash): ~hash;
		struct stats stats;
		printk("%s%3u:%03u hash %8x/%8x ",levels?"":"	", i, block, hash, range);
		if (!(bh = ext3cow_bread (NULL,dir, block, 0,&err))) continue;
		stats = levels?
		   dx_show_entries(hinfo, dir, ((struct dx_node *) bh->b_data)->entries, levels - 1):
		   dx_show_leaf(hinfo, (struct ext3cow_dir_entry_2 *) bh->b_data, blocksize, 0);
		names += stats.names;
		space += stats.space;
		bcount += stats.bcount;
		brelse (bh);
	}
	if (bcount)
		printk("%snames %u, fullness %u (%u%%)\n", levels?"":"	 ",
			names, space/bcount,(space/bcount)*100/blocksize);
	return (struct stats) { names, space, bcount};
}
#endif /* DX_DEBUG */

/*
 * Probe for a directory leaf block to search.
 *
 * dx_probe can return ERR_BAD_DX_DIR, which means there was a format
 * error in the directory index, and the caller should fall back to
 * searching the directory normally.  The callers of dx_probe **MUST**
 * check for this error code, and make sure it never gets reflected
 * back to userspace.
 */
static struct dx_frame *
dx_probe(struct qstr *entry, struct inode *dir,
	 struct dx_hash_info *hinfo, struct dx_frame *frame_in, int *err)
{
	unsigned count, indirect;
	struct dx_entry *at, *entries, *p, *q, *m;
	struct dx_root *root;
	struct buffer_head *bh;
	struct dx_frame *frame = frame_in;
	u32 hash;

	frame->bh = NULL;
	if (!(bh = ext3cow_bread (NULL,dir, 0, 0, err)))
		goto fail;
	root = (struct dx_root *) bh->b_data;
	if (root->info.hash_version != DX_HASH_TEA &&
	    root->info.hash_version != DX_HASH_HALF_MD4 &&
	    root->info.hash_version != DX_HASH_LEGACY) {
		ext3cow_warning(dir->i_sb, __func__,
			     "Unrecognised inode hash code %d",
			     root->info.hash_version);
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}
	hinfo->hash_version = root->info.hash_version;
	if (hinfo->hash_version <= DX_HASH_TEA)
		hinfo->hash_version += EXT3COW_SB(dir->i_sb)->s_hash_unsigned;
	hinfo->seed = EXT3COW_SB(dir->i_sb)->s_hash_seed;
	if (entry)
		ext3cowfs_dirhash(entry->name, entry->len, hinfo);
	hash = hinfo->hash;

	if (root->info.unused_flags & 1) {
		ext3cow_warning(dir->i_sb, __func__,
			     "Unimplemented inode hash flags: %#06x",
			     root->info.unused_flags);
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}

	if ((indirect = root->info.indirect_levels) > 1) {
		ext3cow_warning(dir->i_sb, __func__,
			     "Unimplemented inode hash depth: %#06x",
			     root->info.indirect_levels);
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}

	entries = (struct dx_entry *) (((char *)&root->info) +
				       root->info.info_length);

	if (dx_get_limit(entries) != dx_root_limit(dir,
						   root->info.info_length)) {
		ext3cow_warning(dir->i_sb, __func__,
			     "dx entry: limit != root limit");
		brelse(bh);
		*err = ERR_BAD_DX_DIR;
		goto fail;
	}

	dxtrace (printk("Look up %x", hash));
	while (1)
	{
		count = dx_get_count(entries);
		if (!count || count > dx_get_limit(entries)) {
			ext3cow_warning(dir->i_sb, __func__,
				     "dx entry: no count or count > limit");
			brelse(bh);
			*err = ERR_BAD_DX_DIR;
			goto fail2;
		}

		p = entries + 1;
		q = entries + count - 1;
		while (p <= q)
		{
			m = p + (q - p)/2;
			dxtrace(printk("."));
			if (dx_get_hash(m) > hash)
				q = m - 1;
			else
				p = m + 1;
		}

		if (0) // linear search cross check
		{
			unsigned n = count - 1;
			at = entries;
			while (n--)
			{
				dxtrace(printk(","));
				if (dx_get_hash(++at) > hash)
				{
					at--;
					break;
				}
			}
			assert (at == p - 1);
		}

		at = p - 1;
		dxtrace(printk(" %x->%u\n", at == entries? 0: dx_get_hash(at), dx_get_block(at)));
		frame->bh = bh;
		frame->entries = entries;
		frame->at = at;
		if (!indirect--) return frame;
		if (!(bh = ext3cow_bread (NULL,dir, dx_get_block(at), 0, err)))
			goto fail2;
		at = entries = ((struct dx_node *) bh->b_data)->entries;
		if (dx_get_limit(entries) != dx_node_limit (dir)) {
			ext3cow_warning(dir->i_sb, __func__,
				     "dx entry: limit != node limit");
			brelse(bh);
			*err = ERR_BAD_DX_DIR;
			goto fail2;
		}
		frame++;
		frame->bh = NULL;
	}
fail2:
	while (frame >= frame_in) {
		brelse(frame->bh);
		frame--;
	}
fail:
	if (*err == ERR_BAD_DX_DIR)
		ext3cow_warning(dir->i_sb, __func__,
			     "Corrupt dir inode %ld, running e2fsck is "
			     "recommended.", dir->i_ino);
	return NULL;
}

static void dx_release (struct dx_frame *frames)
{
	if (frames[0].bh == NULL)
		return;

	if (((struct dx_root *) frames[0].bh->b_data)->info.indirect_levels)
		brelse(frames[1].bh);
	brelse(frames[0].bh);
}

/*
 * This function increments the frame pointer to search the next leaf
 * block, and reads in the necessary intervening nodes if the search
 * should be necessary.  Whether or not the search is necessary is
 * controlled by the hash parameter.  If the hash value is even, then
 * the search is only continued if the next block starts with that
 * hash value.	This is used if we are searching for a specific file.
 *
 * If the hash value is HASH_NB_ALWAYS, then always go to the next block.
 *
 * This function returns 1 if the caller should continue to search,
 * or 0 if it should not.  If there is an error reading one of the
 * index blocks, it will a negative error code.
 *
 * If start_hash is non-null, it will be filled in with the starting
 * hash of the next page.
 */
static int ext3cow_htree_next_block(struct inode *dir, __u32 hash,
				 struct dx_frame *frame,
				 struct dx_frame *frames,
				 __u32 *start_hash)
{
	struct dx_frame *p;
	struct buffer_head *bh;
	int err, num_frames = 0;
	__u32 bhash;

	p = frame;
	/*
	 * Find the next leaf page by incrementing the frame pointer.
	 * If we run out of entries in the interior node, loop around and
	 * increment pointer in the parent node.  When we break out of
	 * this loop, num_frames indicates the number of interior
	 * nodes need to be read.
	 */
	while (1) {
		if (++(p->at) < p->entries + dx_get_count(p->entries))
			break;
		if (p == frames)
			return 0;
		num_frames++;
		p--;
	}

	/*
	 * If the hash is 1, then continue only if the next page has a
	 * continuation hash of any value.  This is used for readdir
	 * handling.  Otherwise, check to see if the hash matches the
	 * desired contiuation hash.  If it doesn't, return since
	 * there's no point to read in the successive index pages.
	 */
	bhash = dx_get_hash(p->at);
	if (start_hash)
		*start_hash = bhash;
	if ((hash & 1) == 0) {
		if ((bhash & ~1) != hash)
			return 0;
	}
	/*
	 * If the hash is HASH_NB_ALWAYS, we always go to the next
	 * block so no check is necessary
	 */
	while (num_frames--) {
		if (!(bh = ext3cow_bread(NULL, dir, dx_get_block(p->at),
				      0, &err)))
			return err; /* Failure */
		p++;
		brelse (p->bh);
		p->bh = bh;
		p->at = p->entries = ((struct dx_node *) bh->b_data)->entries;
	}
	return 1;
}


/*
 * This function fills a red-black tree with information from a
 * directory block.  It returns the number directory entries loaded
 * into the tree.  If there is an error it is returned in err.
 */
static int htree_dirblock_to_tree(struct file *dir_file,
				  struct inode *dir, int block,
				  struct dx_hash_info *hinfo,
				  __u32 start_hash, __u32 start_minor_hash)
{
	struct buffer_head *bh;
	struct ext3cow_dir_entry_2 *de, *top;
	int err, count = 0;

	dxtrace(printk("In htree dirblock_to_tree: block %d\n", block));
	if (!(bh = ext3cow_bread (NULL, dir, block, 0, &err)))
		return err;

	de = (struct ext3cow_dir_entry_2 *) bh->b_data;
	top = (struct ext3cow_dir_entry_2 *) ((char *) de +
					   dir->i_sb->s_blocksize -
					   EXT3COW_DIR_REC_LEN(0));
	for (; de < top; de = ext3cow_next_entry(de)) {
		if (!ext3cow_check_dir_entry("htree_dirblock_to_tree", dir, de, bh,
					(block<<EXT3COW_BLOCK_SIZE_BITS(dir->i_sb))
						+((char *)de - bh->b_data))) {
			/* On error, skip the f_pos to the next block. */
			dir_file->f_pos = (dir_file->f_pos |
					(dir->i_sb->s_blocksize - 1)) + 1;
			brelse (bh);
			return count;
		}
		ext3cowfs_dirhash(de->name, de->name_len, hinfo);
		if ((hinfo->hash < start_hash) ||
		    ((hinfo->hash == start_hash) &&
		     (hinfo->minor_hash < start_minor_hash)))
			continue;
		if (de->inode == 0)
			continue;
		if ((err = ext3cow_htree_store_dirent(dir_file,
				   hinfo->hash, hinfo->minor_hash, de)) != 0) {
			brelse(bh);
			return err;
		}
		count++;
	}
	brelse(bh);
	return count;
}


/*
 * This function fills a red-black tree with information from a
 * directory.  We start scanning the directory in hash order, starting
 * at start_hash and start_minor_hash.
 *
 * This function returns the number of entries inserted into the tree,
 * or a negative error code.
 */
int ext3cow_htree_fill_tree(struct file *dir_file, __u32 start_hash,
			 __u32 start_minor_hash, __u32 *next_hash)
{
	struct dx_hash_info hinfo;
	struct ext3cow_dir_entry_2 *de;
	struct dx_frame frames[2], *frame;
	struct inode *dir;
	int block, err;
	int count = 0;
	int ret;
	__u32 hashval;

	dxtrace(printk("In htree_fill_tree, start hash: %x:%x\n", start_hash,
		       start_minor_hash));
	dir = dir_file->f_path.dentry->d_inode;
	if (!(EXT3COW_I(dir)->i_flags & EXT3COW_INDEX_FL)) {
		hinfo.hash_version = EXT3COW_SB(dir->i_sb)->s_def_hash_version;
		if (hinfo.hash_version <= DX_HASH_TEA)
			hinfo.hash_version +=
				EXT3COW_SB(dir->i_sb)->s_hash_unsigned;
		hinfo.seed = EXT3COW_SB(dir->i_sb)->s_hash_seed;
		count = htree_dirblock_to_tree(dir_file, dir, 0, &hinfo,
					       start_hash, start_minor_hash);
		*next_hash = ~0;
		return count;
	}
	hinfo.hash = start_hash;
	hinfo.minor_hash = 0;
	frame = dx_probe(NULL, dir_file->f_path.dentry->d_inode, &hinfo, frames, &err);
	if (!frame)
		return err;

	/* Add '.' and '..' from the htree header */
	if (!start_hash && !start_minor_hash) {
		de = (struct ext3cow_dir_entry_2 *) frames[0].bh->b_data;
		if ((err = ext3cow_htree_store_dirent(dir_file, 0, 0, de)) != 0)
			goto errout;
		count++;
	}
	if (start_hash < 2 || (start_hash ==2 && start_minor_hash==0)) {
		de = (struct ext3cow_dir_entry_2 *) frames[0].bh->b_data;
		de = ext3cow_next_entry(de);
		if ((err = ext3cow_htree_store_dirent(dir_file, 2, 0, de)) != 0)
			goto errout;
		count++;
	}

	while (1) {
		block = dx_get_block(frame->at);
		ret = htree_dirblock_to_tree(dir_file, dir, block, &hinfo,
					     start_hash, start_minor_hash);
		if (ret < 0) {
			err = ret;
			goto errout;
		}
		count += ret;
		hashval = ~0;
		ret = ext3cow_htree_next_block(dir, HASH_NB_ALWAYS,
					    frame, frames, &hashval);
		*next_hash = hashval;
		if (ret < 0) {
			err = ret;
			goto errout;
		}
		/*
		 * Stop if:  (a) there are no more entries, or
		 * (b) we have inserted at least one entry and the
		 * next hash value is not a continuation
		 */
		if ((ret == 0) ||
		    (count && ((hashval & 1) == 0)))
			break;
	}
	dx_release(frames);
	dxtrace(printk("Fill tree: returned %d entries, next hash: %x\n",
		       count, *next_hash));
	return count;
errout:
	dx_release(frames);
	return (err);
}


/*
 * Directory block splitting, compacting
 */

/*
 * Create map of hash values, offsets, and sizes, stored at end of block.
 * Returns number of entries mapped.
 */
static int dx_make_map(struct ext3cow_dir_entry_2 *de, unsigned blocksize,
		struct dx_hash_info *hinfo, struct dx_map_entry *map_tail)
{
	int count = 0;
	char *base = (char *) de;
	struct dx_hash_info h = *hinfo;

	while ((char *) de < base + blocksize)
	{
		if (de->name_len && de->inode) {
			ext3cowfs_dirhash(de->name, de->name_len, &h);
			map_tail--;
			map_tail->hash = h.hash;
			map_tail->offs = (u16) ((char *) de - base);
			map_tail->size = le16_to_cpu(de->rec_len);
			count++;
			cond_resched();
		}
		/* XXX: do we need to check rec_len == 0 case? -Chris */
		de = ext3cow_next_entry(de);
	}
	return count;
}

/* Sort map by hash value */
static void dx_sort_map (struct dx_map_entry *map, unsigned count)
{
	struct dx_map_entry *p, *q, *top = map + count - 1;
	int more;
	/* Combsort until bubble sort doesn't suck */
	while (count > 2)
	{
		count = count*10/13;
		if (count - 9 < 2) /* 9, 10 -> 11 */
			count = 11;
		for (p = top, q = p - count; q >= map; p--, q--)
			if (p->hash < q->hash)
				swap(*p, *q);
	}
	/* Garden variety bubble sort */
	do {
		more = 0;
		q = top;
		while (q-- > map)
		{
			if (q[1].hash >= q[0].hash)
				continue;
			swap(*(q+1), *q);
			more = 1;
		}
	} while(more);
}

static void dx_insert_block(struct dx_frame *frame, u32 hash, u32 block)
{
	struct dx_entry *entries = frame->entries;
	struct dx_entry *old = frame->at, *new = old + 1;
	int count = dx_get_count(entries);

	assert(count < dx_get_limit(entries));
	assert(old < entries + count);
	memmove(new + 1, new, (char *)(entries + count) - (char *)(new));
	dx_set_hash(new, hash);
	dx_set_block(new, block);
	dx_set_count(entries, count + 1);
}

static void ext3cow_update_dx_flag(struct inode *inode)
{
	if (!EXT3COW_HAS_COMPAT_FEATURE(inode->i_sb,
				     EXT3COW_FEATURE_COMPAT_DIR_INDEX))
		EXT3COW_I(inode)->i_flags &= ~EXT3COW_INDEX_FL;
}

/*
 * NOTE! unlike strncmp, ext3cow_match returns 1 for success, 0 for failure.
 *
 * `len <= EXT3COW_NAME_LEN' is guaranteed by caller.
 * `de != NULL' is guaranteed by caller.
 */
static inline int ext3cow_match (int len, const char * const name,
			      struct ext3cow_dir_entry_2 * de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

struct dentry* get_dentry_for_inode(struct inode* inode){
	return d_find_alias(inode);
}   

/*
 * Returns 0 if not found, -1 on failure, and 1 on success
 */
/* For versioning - this is the function used when looking for
    * names.  We now handle names which include the flux token,
    * strip it off and continue looking -znjp */
static inline int search_dirblock(struct buffer_head * bh,
				  struct inode *dir,
				  struct qstr *child,
				  unsigned long offset,
				  struct ext3cow_dir_entry_2 ** res_dir)
{
	struct ext3cow_dir_entry_2 * de;
	char * dlimit, * flux = NULL;
	int de_len;
	char name[EXT3COW_NAME_LEN];
	int namelen = child->len;
	struct dentry *dentry = get_dentry_for_inode(dir);
	unsigned int epoch_number = EXT3COW_I_EPOCHNUMBER(dir);
	
	/* Get the name for the dentry */
	memcpy(name, dentry->d_name.name, namelen);
	name[namelen] = '\0';
	
	/* Check to see if the flux token is in the name */
	flux = strrchr(dentry->d_name.name, EXT3COW_FLUX_TOKEN);
	if(NULL != flux){
	/* If we're here, the name we want is in the past. */
	int new_namelen = strlen(dentry->d_name.name) - strlen(flux);
	/* Get the epoch number */
	epoch_number = simple_strtol(&flux[1], (char **)NULL, 10) - 1;
	/* If there's a valid epoch number or if we're version listing
	 * we need the name seperately, otherwise the FLUX_TOKEN exists
	 * in the file name */
	if(epoch_number + 1 == 0 && (strlen(flux) > 1)){ 
	/* EXT3COW_FLUX_TOKEN exists in the file name */
	    epoch_number = EXT3COW_S_EPOCHNUMBER(dir->i_sb);
	}else{
	    /* Grab the correct name and length */
	    memcpy(name, dentry->d_name.name, new_namelen);
	    name[new_namelen] = '\0';
	    namelen = strlen(name);
	    }
	}
	
	de = (struct ext3cow_dir_entry_2 *) bh->b_data;
	dlimit = bh->b_data + dir->i_sb->s_blocksize;
	while ((char *) de < dlimit) {
		/* this code is executed quadratically often */
		/* do minimal checking `by hand' */

		/* Can't just return first entry of something;
		* may exist twice if died and same name appears again. - znjp
		*/
		if ((char *) de + namelen <= dlimit &&
			ext3cow_match (namelen, name, de) && 
			EXT3COW_IS_DIRENT_SCOPED(de, epoch_number)) {
			/* found a match - just to be sure, do a full check */
			if (!ext3cow_check_dir_entry("ext3cow_find_entry",
						  dir, de, bh, offset))
				return -1;
			*res_dir = de;
			return 1;
		}
		/* prevent looping on a bad block */
		de_len = ext3cow_rec_len_from_disk(de->rec_len);
		if (de_len <= 0)
			return -1;
		offset += de_len;
		de = (struct ext3cow_dir_entry_2 *) ((char *) de + de_len);
	}
	return 0;
}


/*
 *	ext3cow_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * The returned buffer_head has ->b_count elevated.  The caller is expected
 * to brelse() it when appropriate.
 */
static struct buffer_head *ext3cow_find_entry(struct inode *dir,
					struct qstr *entry,
					struct ext3cow_dir_entry_2 **res_dir)
{
	struct super_block * sb;
	struct buffer_head * bh_use[NAMEI_RA_SIZE];
	struct buffer_head * bh, *ret = NULL;
	unsigned long start, block, b;
	const u8 *name = entry->name;
	int ra_max = 0;		/* Number of bh's in the readahead
				   buffer, bh_use[] */
	int ra_ptr = 0;		/* Current index into readahead
				   buffer */
	int num = 0;
	int nblocks, i, err;
	int namelen;

	*res_dir = NULL;
	sb = dir->i_sb;
	namelen = entry->len;
	if (namelen > EXT3COW_NAME_LEN)
		return NULL;
	if ((namelen <= 2) && (name[0] == '.') &&
	    (name[1] == '.' || name[1] == 0)) {
		/*
		 * "." or ".." will only be in the first block
		 * NFS may look up ".."; "." should be handled by the VFS
		 */
		block = start = 0;
		nblocks = 1;
		goto restart;
	}
	if (is_dx(dir)) {
		bh = ext3cow_dx_find_entry(dir, entry, res_dir, &err);
		/*
		 * On success, or if the error was file not found,
		 * return.  Otherwise, fall back to doing a search the
		 * old fashioned way.
		 */
		if (bh || (err != ERR_BAD_DX_DIR))
			return bh;
		dxtrace(printk("ext3cow_find_entry: dx failed, falling back\n"));
	}
	nblocks = dir->i_size >> EXT3COW_BLOCK_SIZE_BITS(sb);
	start = EXT3COW_I(dir)->i_dir_start_lookup;
	if (start >= nblocks)
		start = 0;
	block = start;
restart:
	do {
		/*
		 * We deal with the read-ahead logic here.
		 */
		if (ra_ptr >= ra_max) {
			/* Refill the readahead buffer */
			ra_ptr = 0;
			b = block;
			for (ra_max = 0; ra_max < NAMEI_RA_SIZE; ra_max++) {
				/*
				 * Terminate if we reach the end of the
				 * directory and must wrap, or if our
				 * search has finished at this block.
				 */
				if (b >= nblocks || (num && block == start)) {
					bh_use[ra_max] = NULL;
					break;
				}
				num++;
				bh = ext3cow_getblk(NULL, dir, b++, 0, &err);
				bh_use[ra_max] = bh;
				if (bh && !bh_uptodate_or_lock(bh)) {
					get_bh(bh);
					bh->b_end_io = end_buffer_read_sync;
					submit_bh(READ | REQ_META | REQ_PRIO,
						  bh);
				}
			}
		}
		if ((bh = bh_use[ra_ptr++]) == NULL)
			goto next;
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh)) {
			/* read error, skip block & hope for the best */
			ext3cow_error(sb, __func__, "reading directory #%lu "
				   "offset %lu", dir->i_ino, block);
			brelse(bh);
			goto next;
		}
		i = search_dirblock(bh, dir, entry,
			    block << EXT3COW_BLOCK_SIZE_BITS(sb), res_dir);
		if (i == 1) {
			EXT3COW_I(dir)->i_dir_start_lookup = block;
			ret = bh;
			goto cleanup_and_exit;
		} else {
			brelse(bh);
			if (i < 0)
				goto cleanup_and_exit;
		}
	next:
		if (++block >= nblocks)
			block = 0;
	} while (block != start);

	/*
	 * If the directory has grown while we were searching, then
	 * search the last part of the directory before giving up.
	 */
	block = nblocks;
	nblocks = dir->i_size >> EXT3COW_BLOCK_SIZE_BITS(sb);
	if (block < nblocks) {
		start = 0;
		goto restart;
	}

cleanup_and_exit:
	/* Clean up the read-ahead blocks */
	for (; ra_ptr < ra_max; ra_ptr++)
		brelse (bh_use[ra_ptr]);
	return ret;
}

static struct buffer_head * ext3cow_dx_find_entry(struct inode *dir,
			struct qstr *entry, struct ext3cow_dir_entry_2 **res_dir,
			int *err)
{
	struct super_block *sb = dir->i_sb;
	struct dx_hash_info	hinfo;
	struct dx_frame frames[2], *frame;
	struct buffer_head *bh;
	unsigned long block;
	int retval;

	if (!(frame = dx_probe(entry, dir, &hinfo, frames, err)))
		return NULL;
	do {
		block = dx_get_block(frame->at);
		if (!(bh = ext3cow_bread (NULL,dir, block, 0, err)))
			goto errout;

		retval = search_dirblock(bh, dir, entry,
					 block << EXT3COW_BLOCK_SIZE_BITS(sb),
					 res_dir);
		if (retval == 1) {
			dx_release(frames);
			return bh;
		}
		brelse(bh);
		if (retval == -1) {
			*err = ERR_BAD_DX_DIR;
			goto errout;
		}

		/* Check to see if we should continue to search */
		retval = ext3cow_htree_next_block(dir, hinfo.hash, frame,
					       frames, NULL);
		if (retval < 0) {
			ext3cow_warning(sb, __func__,
			     "error reading index page in directory #%lu",
			     dir->i_ino);
			*err = retval;
			goto errout;
		}
	} while (retval == 1);

	*err = -ENOENT;
errout:
	dxtrace(printk("%s not found\n", entry->name));
	dx_release (frames);
	return NULL;
}

static struct dentry *ext3cow_lookup(struct inode * dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode * inode = NULL;
	struct ext3cow_dir_entry_2 * de = NULL;
	struct buffer_head * bh = NULL;
	unsigned int epoch_number = 0;
	char * flux = NULL;

	if (dentry->d_name.len > EXT3COW_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);
	/* Find the epoch number to scope with -znjp 
	* if the parent is unchangeable, so is the inode 
	*/
	if(EXT3COW_IS_UNCHANGEABLE(dir))	  
	    epoch_number = EXT3COW_I_EPOCHNUMBER(dir);
	else
	    epoch_number = EXT3COW_S_EPOCHNUMBER(dir->i_sb);

	bh = ext3cow_find_entry(dir, &dentry->d_name, &de);
	if (bh) {
		unsigned long ino = le32_to_cpu(de->inode);
		brelse (bh);
		if (!ext3cow_valid_inum(dir->i_sb, ino)) {
			ext3cow_error(dir->i_sb, "ext3cow_lookup",
				   "bad inode number: %lu", ino);
			return ERR_PTR(-EIO);
		}
		inode = ext3cow_iget(dir->i_sb, ino);
		if (inode == ERR_PTR(-ESTALE)) {
			ext3cow_error(dir->i_sb, __func__,
					"deleted inode referenced: %lu",
					ino);
			return ERR_PTR(-EIO);
		}

		/* Is this a version listing ? */
		if ((char)dentry->d_name.name[dentry->d_name.len - 1] == 
		    EXT3COW_FLUX_TOKEN) {
		    /* prevent going round in circles */
		    if (dentry->d_parent && 
			      dentry->d_parent->d_name.name[dentry->d_parent->d_name.len - 1] ==
			      EXT3COW_FLUX_TOKEN) {
			return NULL;
		      }
	      /* we fake a directory using the directory inode instead of
			 * the file one and subsequently force a call to ext3cow_readdir */
		    iput(inode);
		    inode = ext3cow_fake_inode(dir, EXT3COW_S_EPOCHNUMBER(dir->i_sb));
		    EXT3COW_I(inode)->i_next_inode = EXT3COW_I(dir)->i_next_inode;
		    d_splice_alias(inode, dentry);
	      
		    return NULL;
		    }
		 /* Is the user time-shifting to the past? */
    flux = strrchr(dentry->d_name.name, EXT3COW_FLUX_TOKEN);
    if(NULL != flux){

      if(strnicmp(&flux[1], "onehour", 8) == 0){
        epoch_number = get_seconds() - ONEHOUR;
        printk(KERN_INFO "ONEHOUR!\n");
      }else if(strnicmp(&flux[1], "yesterday", 10) == 0 ||
               strnicmp(&flux[1], "oneday", 7) == 0){
        epoch_number = get_seconds() - YESTERDAY;
      }else if(strnicmp(&flux[1], "oneweek", 8) == 0){
        epoch_number = get_seconds() - ONEWEEK;
      }else if(strnicmp(&flux[1], "onemonth", 9) == 0){
        epoch_number = get_seconds() - ONEMONTH;
      }else if(strnicmp(&flux[1], "oneyear", 8) == 0){
        epoch_number = get_seconds() - ONEYEAR;
      }else
        epoch_number = simple_strtol(&flux[1], (char **)NULL, 10) - 1;

      /* No future epochs */
      if(epoch_number + 1 > EXT3COW_S_EPOCHNUMBER(dir->i_sb))                
        return ERR_PTR(-ENOENT); 

      /* Move to present 
      if(epoch_number + 1 == 0)
        epoch_number = EXT3COW_S_EPOCHNUMBER(dir->i_sb);       
      */
    }
    /* Find correct inode in chain */
    while(EXT3COW_I_EPOCHNUMBER(inode) > epoch_number){

      printk(KERN_INFO "Looking for %u with epoch %u\n", epoch_number, 
             EXT3COW_I_EPOCHNUMBER(inode));

      ino = EXT3COW_I(inode)->i_next_inode;
      if(ino == 0){
        ext3cow_warning(dir->i_sb, "ext3cow_lookup",
                        "Next inode is 0 in lookup.");
        iput(inode);
        return ERR_PTR(-ENOENT);
      }
      iput(inode); /* for correct usage count (i_count) */
      inode = ext3cow_iget(dir->i_sb, ino);
      
      if (!inode){
        ext3cow_warning(dir->i_sb, "ext3cow_lookup",
                        "Could not access inode number %lu",
                        ino);
        return ERR_PTR(-EACCES);
      }
    }

    /* If we're in the past, fake the inode for scoping and "unchangability" */
       if(flux || (epoch_number != EXT3COW_S_EPOCHNUMBER(dir->i_sb))){
      printk(KERN_INFO "Faking %s\n", dentry->d_name.name);
      inode = ext3cow_fake_inode(inode, epoch_number);
    }

    if (!inode)
      return ERR_PTR(-EACCES);

    }
	return d_splice_alias(inode, dentry);
}


struct dentry *ext3cow_get_parent(struct dentry *child)
{
	unsigned long ino;
	struct qstr dotdot = {.name = "..", .len = 2};
	struct ext3cow_dir_entry_2 * de;
	struct buffer_head *bh;

	bh = ext3cow_find_entry(child->d_inode, &dotdot, &de);
	if (!bh)
		return ERR_PTR(-ENOENT);
	ino = le32_to_cpu(de->inode);
	brelse(bh);

	if (!ext3cow_valid_inum(child->d_inode->i_sb, ino)) {
		ext3cow_error(child->d_inode->i_sb, "ext3cow_get_parent",
			   "bad inode number: %lu", ino);
		return ERR_PTR(-EIO);
	}

	return d_obtain_alias(ext3cow_iget(child->d_inode->i_sb, ino));
}

#define S_SHIFT 12
static unsigned char ext3cow_type_by_mode[S_IFMT >> S_SHIFT] = {
	[S_IFREG >> S_SHIFT]	= EXT3COW_FT_REG_FILE,
	[S_IFDIR >> S_SHIFT]	= EXT3COW_FT_DIR,
	[S_IFCHR >> S_SHIFT]	= EXT3COW_FT_CHRDEV,
	[S_IFBLK >> S_SHIFT]	= EXT3COW_FT_BLKDEV,
	[S_IFIFO >> S_SHIFT]	= EXT3COW_FT_FIFO,
	[S_IFSOCK >> S_SHIFT]	= EXT3COW_FT_SOCK,
	[S_IFLNK >> S_SHIFT]	= EXT3COW_FT_SYMLINK,
};

static inline void ext3cow_set_de_type(struct super_block *sb,
				struct ext3cow_dir_entry_2 *de,
				umode_t mode) {
	if (EXT3COW_HAS_INCOMPAT_FEATURE(sb, EXT3COW_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = ext3cow_type_by_mode[(mode & S_IFMT)>>S_SHIFT];
}

/*
 * Move count entries from end of map between two memory locations.
 * Returns pointer to last entry moved.
 */
static struct ext3cow_dir_entry_2 *
dx_move_dirents(char *from, char *to, struct dx_map_entry *map, int count)
{
	unsigned rec_len = 0;

	while (count--) {
		struct ext3cow_dir_entry_2 *de = (struct ext3cow_dir_entry_2 *) (from + map->offs);
		rec_len = EXT3COW_DIR_REC_LEN(de->name_len);
		memcpy (to, de, rec_len);
		((struct ext3cow_dir_entry_2 *) to)->rec_len =
				ext3cow_rec_len_to_disk(rec_len);
		de->inode = 0;
		map++;
		to += rec_len;
	}
	return (struct ext3cow_dir_entry_2 *) (to - rec_len);
}

/*
 * Compact each dir entry in the range to the minimal rec_len.
 * Returns pointer to last entry in range.
 */
static struct ext3cow_dir_entry_2 *dx_pack_dirents(char *base, unsigned blocksize)
{
	struct ext3cow_dir_entry_2 *next, *to, *prev;
	struct ext3cow_dir_entry_2 *de = (struct ext3cow_dir_entry_2 *)base;
	unsigned rec_len = 0;

	prev = to = de;
	while ((char *)de < base + blocksize) {
		next = ext3cow_next_entry(de);
		if (de->inode && de->name_len) {
			rec_len = EXT3COW_DIR_REC_LEN(de->name_len);
			if (de > to)
				memmove(to, de, rec_len);
			to->rec_len = ext3cow_rec_len_to_disk(rec_len);
			prev = to;
			to = (struct ext3cow_dir_entry_2 *) (((char *) to) + rec_len);
		}
		de = next;
	}
	return prev;
}

/*
 * Split a full leaf block to make room for a new dir entry.
 * Allocate a new block, and move entries so that they are approx. equally full.
 * Returns pointer to de in block into which the new entry will be inserted.
 */
static struct ext3cow_dir_entry_2 *do_split(handle_t *handle, struct inode *dir,
			struct buffer_head **bh,struct dx_frame *frame,
			struct dx_hash_info *hinfo, int *error)
{
	unsigned blocksize = dir->i_sb->s_blocksize;
	unsigned count, continued;
	struct buffer_head *bh2;
	u32 newblock;
	u32 hash2;
	struct dx_map_entry *map;
	char *data1 = (*bh)->b_data, *data2;
	unsigned split, move, size;
	struct ext3cow_dir_entry_2 *de = NULL, *de2;
	int	err = 0, i;

	bh2 = ext3cow_append (handle, dir, &newblock, &err);
	if (!(bh2)) {
		brelse(*bh);
		*bh = NULL;
		goto errout;
	}

	BUFFER_TRACE(*bh, "get_write_access");
	err = ext3cow_journal_get_write_access(handle, *bh);
	if (err)
		goto journal_error;

	BUFFER_TRACE(frame->bh, "get_write_access");
	err = ext3cow_journal_get_write_access(handle, frame->bh);
	if (err)
		goto journal_error;

	data2 = bh2->b_data;

	/* create map in the end of data2 block */
	map = (struct dx_map_entry *) (data2 + blocksize);
	count = dx_make_map ((struct ext3cow_dir_entry_2 *) data1,
			     blocksize, hinfo, map);
	map -= count;
	dx_sort_map (map, count);
	/* Split the existing block in the middle, size-wise */
	size = 0;
	move = 0;
	for (i = count-1; i >= 0; i--) {
		/* is more than half of this entry in 2nd half of the block? */
		if (size + map[i].size/2 > blocksize/2)
			break;
		size += map[i].size;
		move++;
	}
	/* map index at which we will split */
	split = count - move;
	hash2 = map[split].hash;
	continued = hash2 == map[split - 1].hash;
	dxtrace(printk("Split block %i at %x, %i/%i\n",
		dx_get_block(frame->at), hash2, split, count-split));

	/* Fancy dance to stay within two buffers */
	de2 = dx_move_dirents(data1, data2, map + split, count - split);
	de = dx_pack_dirents(data1,blocksize);
	de->rec_len = ext3cow_rec_len_to_disk(data1 + blocksize - (char *) de);
	de2->rec_len = ext3cow_rec_len_to_disk(data2 + blocksize - (char *) de2);
	dxtrace(dx_show_leaf (hinfo, (struct ext3cow_dir_entry_2 *) data1, blocksize, 1));
	dxtrace(dx_show_leaf (hinfo, (struct ext3cow_dir_entry_2 *) data2, blocksize, 1));

	/* Which block gets the new entry? */
	if (hinfo->hash >= hash2)
	{
		swap(*bh, bh2);
		de = de2;
	}
	dx_insert_block (frame, hash2 + continued, newblock);
	err = ext3cow_journal_dirty_metadata (handle, bh2);
	if (err)
		goto journal_error;
	err = ext3cow_journal_dirty_metadata (handle, frame->bh);
	if (err)
		goto journal_error;
	brelse (bh2);
	dxtrace(dx_show_index ("frame", frame->entries));
	return de;

journal_error:
	brelse(*bh);
	brelse(bh2);
	*bh = NULL;
	ext3cow_std_error(dir->i_sb, err);
errout:
	*error = err;
	return NULL;
}


/*
 * Add a new entry into a directory (leaf) block.  If de is non-NULL,
 * it points to a directory entry which is guaranteed to be large
 * enough for new directory entry.  If de is NULL, then
 * add_dirent_to_buf will attempt search the directory block for
 * space.  It will return -ENOSPC if no space is available, and -EIO
 * and -EEXIST if directory entry already exists.
 *
 * NOTE!  bh is NOT released in the case where ENOSPC is returned.  In
 * all other cases bh is released.
 */
static int add_dirent_to_buf(handle_t *handle, struct dentry *dentry,
			     struct inode *inode, struct ext3cow_dir_entry_2 *de,
			     struct buffer_head * bh)
{
	struct inode	*dir = dentry->d_parent->d_inode;
	const char	*name = dentry->d_name.name;
	int		namelen = dentry->d_name.len;
	unsigned long	offset = 0;
	unsigned short	reclen;
	int		nlen, rlen, err;
	char		*top;

	reclen = EXT3COW_DIR_REC_LEN(namelen);
	if (!de) {
		de = (struct ext3cow_dir_entry_2 *)bh->b_data;
		top = bh->b_data + dir->i_sb->s_blocksize - reclen;
		while ((char *) de <= top) {
			if (!ext3cow_check_dir_entry("ext3cow_add_entry", dir, de,
						  bh, offset)) {
				brelse (bh);
				ext3cow_reclaim_dup_inode(dentry->d_parent->d_parent->d_inode, dir);
				return -EIO;
			}
			if (ext3cow_match (namelen, name, de) && EXT3COW_IS_DIRENT_ALIVE(de)) {
				brelse (bh);
				return -EEXIST;
			}
			nlen = EXT3COW_DIR_REC_LEN(de->name_len);
			rlen = ext3cow_rec_len_from_disk(de->rec_len);
			if ((de->inode? rlen - nlen: rlen) >= reclen)
				break;
			de = (struct ext3cow_dir_entry_2 *)((char *)de + rlen);
			offset += rlen;
		}
		if ((char *) de > top)
			return -ENOSPC;
	}
	BUFFER_TRACE(bh, "get_write_access");
	err = ext3cow_journal_get_write_access(handle, bh);
	if (err) {
		ext3cow_std_error(dir->i_sb, err);
		brelse(bh);
		return err;
	}

	/* By now the buffer is marked for journaling */
	nlen = EXT3COW_DIR_REC_LEN(de->name_len);
	rlen = ext3cow_rec_len_from_disk(de->rec_len);
	if (de->inode) {
		struct ext3cow_dir_entry_2 *de1 = (struct ext3cow_dir_entry_2 *)((char *)de + nlen);
		de1->rec_len = ext3cow_rec_len_to_disk(rlen - nlen);
		de->rec_len = ext3cow_rec_len_to_disk(nlen);
		de = de1;
	}
	de->file_type = EXT3COW_FT_UNKNOWN;
	if (inode) {
		de->inode = cpu_to_le32(inode->i_ino);
		ext3cow_set_de_type(dir->i_sb, de, inode->i_mode);
	} else
		de->inode = 0;
		/* For versioning -znjp */
		de->birth_epoch = cpu_to_le32(EXT3COW_S_EPOCHNUMBER(dir->i_sb));
		de->death_epoch = cpu_to_le32(EXT3COW_DIRENT_ALIVE);
	de->name_len = namelen;
	memcpy (de->name, name, namelen);
	/*
	 * XXX shouldn't update any times until successful
	 * completion of syscall, but too many callers depend
	 * on this.
	 *
	 * XXX similarly, too many callers depend on
	 * ext3cow_new_inode() setting the times, but error
	 * recovery deletes the inode, so the worst that can
	 * happen is that the times are slightly out of date
	 * and/or different from the directory change time.
	 */
	dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
	ext3cow_update_dx_flag(dir);
	dir->i_version++;
	ext3cow_mark_inode_dirty(handle, dir);
	BUFFER_TRACE(bh, "call ext3cow_journal_dirty_metadata");
	err = ext3cow_journal_dirty_metadata(handle, bh);
	if (err)
		ext3cow_std_error(dir->i_sb, err);
	brelse(bh);
	return 0;
}

/*
 * This converts a one block unindexed directory to a 3 block indexed
 * directory, and adds the dentry to the indexed directory.
 */
static int make_indexed_dir(handle_t *handle, struct dentry *dentry,
			    struct inode *inode, struct buffer_head *bh)
{
	struct inode	*dir = dentry->d_parent->d_inode;
	const char	*name = dentry->d_name.name;
	int		namelen = dentry->d_name.len;
	struct buffer_head *bh2;
	struct dx_root	*root;
	struct dx_frame	frames[2], *frame;
	struct dx_entry *entries;
	struct ext3cow_dir_entry_2	*de, *de2;
	char		*data1, *top;
	unsigned	len;
	int		retval;
	unsigned	blocksize;
	struct dx_hash_info hinfo;
	u32		block;
	struct fake_dirent *fde;

	blocksize =  dir->i_sb->s_blocksize;
	dxtrace(printk(KERN_DEBUG "Creating index: inode %lu\n", dir->i_ino));
	retval = ext3cow_journal_get_write_access(handle, bh);
	if (retval) {
		ext3cow_std_error(dir->i_sb, retval);
		brelse(bh);
		return retval;
	}
	root = (struct dx_root *) bh->b_data;

	/* The 0th block becomes the root, move the dirents out */
	fde = &root->dotdot;
	de = (struct ext3cow_dir_entry_2 *)((char *)fde +
			ext3cow_rec_len_from_disk(fde->rec_len));
	if ((char *) de >= (((char *) root) + blocksize)) {
		ext3cow_error(dir->i_sb, __func__,
			   "invalid rec_len for '..' in inode %lu",
			   dir->i_ino);
		brelse(bh);
		return -EIO;
	}
	len = ((char *) root) + blocksize - (char *) de;

	bh2 = ext3cow_append (handle, dir, &block, &retval);
	if (!(bh2)) {
		brelse(bh);
		return retval;
	}
	EXT3COW_I(dir)->i_flags |= EXT3COW_INDEX_FL;
	data1 = bh2->b_data;

	memcpy (data1, de, len);
	de = (struct ext3cow_dir_entry_2 *) data1;
	top = data1 + len;
	while ((char *)(de2 = ext3cow_next_entry(de)) < top)
		de = de2;
	de->rec_len = ext3cow_rec_len_to_disk(data1 + blocksize - (char *) de);
	/* Initialize the root; the dot dirents already exist */
	de = (struct ext3cow_dir_entry_2 *) (&root->dotdot);
	de->rec_len = ext3cow_rec_len_to_disk(blocksize - EXT3COW_DIR_REC_LEN(2));
	memset (&root->info, 0, sizeof(root->info));
	root->info.info_length = sizeof(root->info);
	root->info.hash_version = EXT3COW_SB(dir->i_sb)->s_def_hash_version;
	entries = root->entries;
	dx_set_block (entries, 1);
	dx_set_count (entries, 1);
	dx_set_limit (entries, dx_root_limit(dir, sizeof(root->info)));

	/* Initialize as for dx_probe */
	hinfo.hash_version = root->info.hash_version;
	if (hinfo.hash_version <= DX_HASH_TEA)
		hinfo.hash_version += EXT3COW_SB(dir->i_sb)->s_hash_unsigned;
	hinfo.seed = EXT3COW_SB(dir->i_sb)->s_hash_seed;
	ext3cowfs_dirhash(name, namelen, &hinfo);
	frame = frames;
	frame->entries = entries;
	frame->at = entries;
	frame->bh = bh;
	bh = bh2;
	/*
	 * Mark buffers dirty here so that if do_split() fails we write a
	 * consistent set of buffers to disk.
	 */
	ext3cow_journal_dirty_metadata(handle, frame->bh);
	ext3cow_journal_dirty_metadata(handle, bh);
	de = do_split(handle,dir, &bh, frame, &hinfo, &retval);
	if (!de) {
		ext3cow_mark_inode_dirty(handle, dir);
		dx_release(frames);
		return retval;
	}
	dx_release(frames);

	return add_dirent_to_buf(handle, dentry, inode, de, bh);
}

/*
 *	ext3cow_add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as ext3cow_find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static int ext3cow_add_entry (handle_t *handle, struct dentry *dentry,
	struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode;
	struct buffer_head * bh;
	struct ext3cow_dir_entry_2 *de;
	struct super_block * sb;
	int	retval;
	int	dx_fallback=0;
	unsigned blocksize;
	u32 block, blocks;

	sb = dir->i_sb;
	blocksize = sb->s_blocksize;
	if (!dentry->d_name.len)
		return -EINVAL;
  /* No additions in the past -znjp */
  if(is_unchangeable(dir, dentry))
    return -EROFS;

  if(EXT3COW_S_EPOCHNUMBER(sb) > EXT3COW_I_EPOCHNUMBER(dir)){   
    if(ext3cow_dup_inode(dentry->d_parent->d_parent->d_inode, dir))
      return -1;
}
	if (is_dx(dir)) {
		retval = ext3cow_dx_add_entry(handle, dentry, inode);
               if (!retval || (retval != ERR_BAD_DX_DIR)){
		    ext3cow_reclaim_dup_inode(dentry->d_parent->d_parent->d_inode, dir);
		    return retval;
		}
		EXT3COW_I(dir)->i_flags &= ~EXT3COW_INDEX_FL;
		dx_fallback++;
		ext3cow_mark_inode_dirty(handle, dir);
	}
	blocks = dir->i_size >> sb->s_blocksize_bits;
	for (block = 0; block < blocks; block++) {
		bh = ext3cow_bread(handle, dir, block, 0, &retval);
		if(!bh){
			ext3cow_reclaim_dup_inode(dentry->d_parent->d_parent->d_inode, dir);
			return retval;
		}
		retval = add_dirent_to_buf(handle, dentry, inode, NULL, bh);
		if (retval != -ENOSPC)
			return retval;

		if (blocks == 1 && !dx_fallback &&
		    EXT3COW_HAS_COMPAT_FEATURE(sb, EXT3COW_FEATURE_COMPAT_DIR_INDEX))
			return make_indexed_dir(handle, dentry, inode, bh);
		brelse(bh);
	}
	bh = ext3cow_append(handle, dir, &block, &retval);
	if (!bh){
		ext3cow_reclaim_dup_inode(dentry->d_parent->d_parent->d_inode, dir);
		return retval;
	}
	de = (struct ext3cow_dir_entry_2 *) bh->b_data;
	de->inode = 0;
	de->rec_len = ext3cow_rec_len_to_disk(blocksize);
	return add_dirent_to_buf(handle, dentry, inode, de, bh);
}

/*
 * Returns 0 for success, or a negative error value
 */
static int ext3cow_dx_add_entry(handle_t *handle, struct dentry *dentry,
			     struct inode *inode)
{
	struct dx_frame frames[2], *frame;
	struct dx_entry *entries, *at;
	struct dx_hash_info hinfo;
	struct buffer_head * bh;
	struct inode *dir = dentry->d_parent->d_inode;
	struct super_block * sb = dir->i_sb;
	struct ext3cow_dir_entry_2 *de;
	int err;

	frame = dx_probe(&dentry->d_name, dir, &hinfo, frames, &err);
	if (!frame)
		return err;
	entries = frame->entries;
	at = frame->at;

	if (!(bh = ext3cow_bread(handle,dir, dx_get_block(frame->at), 0, &err)))
		goto cleanup;

	BUFFER_TRACE(bh, "get_write_access");
	err = ext3cow_journal_get_write_access(handle, bh);
	if (err)
		goto journal_error;

	err = add_dirent_to_buf(handle, dentry, inode, NULL, bh);
	if (err != -ENOSPC) {
		bh = NULL;
		goto cleanup;
	}

	/* Block full, should compress but for now just split */
	dxtrace(printk("using %u of %u node entries\n",
		       dx_get_count(entries), dx_get_limit(entries)));
	/* Need to split index? */
	if (dx_get_count(entries) == dx_get_limit(entries)) {
		u32 newblock;
		unsigned icount = dx_get_count(entries);
		int levels = frame - frames;
		struct dx_entry *entries2;
		struct dx_node *node2;
		struct buffer_head *bh2;

		if (levels && (dx_get_count(frames->entries) ==
			       dx_get_limit(frames->entries))) {
			ext3cow_warning(sb, __func__,
				     "Directory index full!");
			err = -ENOSPC;
			goto cleanup;
		}
		bh2 = ext3cow_append (handle, dir, &newblock, &err);
		if (!(bh2))
			goto cleanup;
		node2 = (struct dx_node *)(bh2->b_data);
		entries2 = node2->entries;
		memset(&node2->fake, 0, sizeof(struct fake_dirent));
		node2->fake.rec_len = ext3cow_rec_len_to_disk(sb->s_blocksize);
		BUFFER_TRACE(frame->bh, "get_write_access");
		err = ext3cow_journal_get_write_access(handle, frame->bh);
		if (err)
			goto journal_error;
		if (levels) {
			unsigned icount1 = icount/2, icount2 = icount - icount1;
			unsigned hash2 = dx_get_hash(entries + icount1);
			dxtrace(printk("Split index %i/%i\n", icount1, icount2));

			BUFFER_TRACE(frame->bh, "get_write_access"); /* index root */
			err = ext3cow_journal_get_write_access(handle,
							     frames[0].bh);
			if (err)
				goto journal_error;

			memcpy ((char *) entries2, (char *) (entries + icount1),
				icount2 * sizeof(struct dx_entry));
			dx_set_count (entries, icount1);
			dx_set_count (entries2, icount2);
			dx_set_limit (entries2, dx_node_limit(dir));

			/* Which index block gets the new entry? */
			if (at - entries >= icount1) {
				frame->at = at = at - entries - icount1 + entries2;
				frame->entries = entries = entries2;
				swap(frame->bh, bh2);
			}
			dx_insert_block (frames + 0, hash2, newblock);
			dxtrace(dx_show_index ("node", frames[1].entries));
			dxtrace(dx_show_index ("node",
			       ((struct dx_node *) bh2->b_data)->entries));
			err = ext3cow_journal_dirty_metadata(handle, bh2);
			if (err)
				goto journal_error;
			brelse (bh2);
		} else {
			dxtrace(printk("Creating second level index...\n"));
			memcpy((char *) entries2, (char *) entries,
			       icount * sizeof(struct dx_entry));
			dx_set_limit(entries2, dx_node_limit(dir));

			/* Set up root */
			dx_set_count(entries, 1);
			dx_set_block(entries + 0, newblock);
			((struct dx_root *) frames[0].bh->b_data)->info.indirect_levels = 1;

			/* Add new access path frame */
			frame = frames + 1;
			frame->at = at = at - entries + entries2;
			frame->entries = entries = entries2;
			frame->bh = bh2;
			err = ext3cow_journal_get_write_access(handle,
							     frame->bh);
			if (err)
				goto journal_error;
		}
		err = ext3cow_journal_dirty_metadata(handle, frames[0].bh);
		if (err)
			goto journal_error;
	}
	de = do_split(handle, dir, &bh, frame, &hinfo, &err);
	if (!de)
		goto cleanup;
	err = add_dirent_to_buf(handle, dentry, inode, de, bh);
	bh = NULL;
	goto cleanup;

journal_error:
	ext3cow_std_error(dir->i_sb, err);
cleanup:
	if (bh)
		brelse(bh);
	dx_release(frames);
	return err;
}

/*
 * ext3cow_delete_entry deletes a directory entry by merging it with the
 * previous entry
 */
static int ext3cow_delete_entry (handle_t *handle,
			      struct inode * dir,
			      struct ext3cow_dir_entry_2 * de_del,
			      struct buffer_head * bh,
			      struct dentry *dentry)
{
	struct ext3cow_dir_entry_2 * de, * pde;
	int i;

	i = 0;
	pde = NULL;
	de = (struct ext3cow_dir_entry_2 *) bh->b_data;
	while (i < bh->b_size) {
		if (!ext3cow_check_dir_entry("ext3cow_delete_entry", dir, de, bh, i))
			return -EIO;
		if (de == de_del)  {
			int err;
			/* Can't delete an already dead entry - znjp */
      if(!EXT3COW_IS_DIRENT_ALIVE(de))
        return 0;
      
      if(EXT3COW_S_EPOCHNUMBER(dir->i_sb) > EXT3COW_I_EPOCHNUMBER(dir)){
        if(ext3cow_dup_inode(dentry->d_parent->d_parent->d_inode, dir))
          return -1;
      }

			BUFFER_TRACE(bh, "get_write_access");
			err = ext3cow_journal_get_write_access(handle, bh);
			if (err)
				goto journal_error;

			if (pde)
				pde->rec_len = ext3cow_rec_len_to_disk(
					ext3cow_rec_len_from_disk(pde->rec_len) +
					ext3cow_rec_len_from_disk(de->rec_len));
			else
				de->inode = 0;
			/* Mark it dead - znjp */
			de->death_epoch = cpu_to_le32(EXT3COW_I_EPOCHNUMBER(dir));

			dir->i_version++;
			BUFFER_TRACE(bh, "call ext3cow_journal_dirty_metadata");
			err = ext3cow_journal_dirty_metadata(handle, bh);
			if (err) {
journal_error:
				ext3cow_std_error(dir->i_sb, err);
				return err;
			}
			return 0;
		}
		i += ext3cow_rec_len_from_disk(de->rec_len);
		pde = de;
		de = ext3cow_next_entry(de);
	}
	return -ENOENT;
}

static int ext3cow_add_nondir(handle_t *handle,
		struct dentry *dentry, struct inode *inode)
{
	int err = ext3cow_add_entry(handle, dentry, inode);
	if (!err) {
		ext3cow_mark_inode_dirty(handle, inode);
		d_instantiate(dentry, inode);
		unlock_new_inode(inode);
		return 0;
	}
	drop_nlink(inode);
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int ext3cow_create (struct inode * dir, struct dentry * dentry, umode_t mode,
		struct nameidata *nd)
{
	handle_t *handle;
	struct inode * inode;
	int err, retries = 0;
	/* Can't create in the past -znjp */
	if(is_unchangeable(dir, dentry))
	    return -EROFS;
	dquot_initialize(dir);

retry:
	handle = ext3cow_journal_start(dir, EXT3COW_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT3COW_INDEX_EXTRA_TRANS_BLOCKS + 3 +
					EXT3COW_MAXQUOTAS_INIT_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	inode = ext3cow_new_inode (handle, dir, &dentry->d_name, mode);
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		inode->i_op = &ext3cow_file_inode_operations;
		inode->i_fop = &ext3cow_file_operations;
		ext3cow_set_aops(inode);
		err = ext3cow_add_nondir(handle, dentry, inode);
	}
	ext3cow_journal_stop(handle);
	if (err == -ENOSPC && ext3cow_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

static int ext3cow_mknod (struct inode * dir, struct dentry *dentry,
			umode_t mode, dev_t rdev)
{
	handle_t *handle;
	struct inode *inode;
	int err, retries = 0;

	if (!new_valid_dev(rdev))
		return -EINVAL;

	dquot_initialize(dir);

retry:
	handle = ext3cow_journal_start(dir, EXT3COW_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT3COW_INDEX_EXTRA_TRANS_BLOCKS + 3 +
					EXT3COW_MAXQUOTAS_INIT_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	inode = ext3cow_new_inode (handle, dir, &dentry->d_name, mode);
	err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		init_special_inode(inode, inode->i_mode, rdev);
#ifdef CONFIG_EXT3COW_FS_XATTR
		inode->i_op = &ext3cow_special_inode_operations;
#endif
		err = ext3cow_add_nondir(handle, dentry, inode);
	}
	ext3cow_journal_stop(handle);
	if (err == -ENOSPC && ext3cow_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

static int ext3cow_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode)
{
	handle_t *handle;
	struct inode * inode;
	struct buffer_head * dir_block = NULL;
	struct ext3cow_dir_entry_2 * de;
	int err, retries = 0;

	if (dir->i_nlink >= EXT3COW_LINK_MAX)
		return -EMLINK;
	/* No mkdirs in the past -znjp */
	if(is_unchangeable(dir, dentry))
	    return -EROFS;


	dquot_initialize(dir);

retry:
	handle = ext3cow_journal_start(dir, EXT3COW_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT3COW_INDEX_EXTRA_TRANS_BLOCKS + 3 +
					EXT3COW_MAXQUOTAS_INIT_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	inode = ext3cow_new_inode (handle, dir, &dentry->d_name, S_IFDIR | mode);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_stop;

	inode->i_op = &ext3cow_dir_inode_operations;
	inode->i_fop = &ext3cow_dir_operations;
	inode->i_size = EXT3COW_I(inode)->i_disksize = inode->i_sb->s_blocksize;
	dir_block = ext3cow_bread (handle, inode, 0, 1, &err);
	if (!dir_block)
		goto out_clear_inode;

	BUFFER_TRACE(dir_block, "get_write_access");
	err = ext3cow_journal_get_write_access(handle, dir_block);
	if (err)
		goto out_clear_inode;

	de = (struct ext3cow_dir_entry_2 *) dir_block->b_data;
	de->inode = cpu_to_le32(inode->i_ino);
	de->name_len = 1;
	de->rec_len = ext3cow_rec_len_to_disk(EXT3COW_DIR_REC_LEN(de->name_len));
	/* For versioning -znjp */
	de->birth_epoch = cpu_to_le32(EXT3COW_S_EPOCHNUMBER(dir->i_sb));
	de->death_epoch = cpu_to_le32(EXT3COW_DIRENT_ALIVE);
	strcpy (de->name, ".");
	ext3cow_set_de_type(dir->i_sb, de, S_IFDIR);
	de = ext3cow_next_entry(de);
	de->inode = cpu_to_le32(dir->i_ino);
	de->rec_len = ext3cow_rec_len_to_disk(inode->i_sb->s_blocksize -
					EXT3COW_DIR_REC_LEN(1));
	de->name_len = 2;
	strcpy (de->name, "..");
	ext3cow_set_de_type(dir->i_sb, de, S_IFDIR);
	set_nlink(inode, 2);
	/* For versioning -znjp */
	de->birth_epoch = cpu_to_le32(EXT3COW_I_EPOCHNUMBER(dir)); 
	de->death_epoch = cpu_to_le32(EXT3COW_DIRENT_ALIVE);
	BUFFER_TRACE(dir_block, "call ext3cow_journal_dirty_metadata");
	err = ext3cow_journal_dirty_metadata(handle, dir_block);
	if (err)
		goto out_clear_inode;

	err = ext3cow_mark_inode_dirty(handle, inode);
	if (!err)
		err = ext3cow_add_entry (handle, dentry, inode);

	if (err) {
out_clear_inode:
		clear_nlink(inode);
		unlock_new_inode(inode);
		ext3cow_mark_inode_dirty(handle, inode);
		iput (inode);
		goto out_stop;
	}
	inc_nlink(dir);
	ext3cow_update_dx_flag(dir);
	err = ext3cow_mark_inode_dirty(handle, dir);
	if (err)
		goto out_clear_inode;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
out_stop:
	brelse(dir_block);
	ext3cow_journal_stop(handle);
	if (err == -ENOSPC && ext3cow_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir (struct inode * inode)
{
	unsigned long offset;
	struct buffer_head * bh;
	struct ext3cow_dir_entry_2 * de, * de1;
	struct super_block * sb;
	int err = 0;

	sb = inode->i_sb;
	if (inode->i_size < EXT3COW_DIR_REC_LEN(1) + EXT3COW_DIR_REC_LEN(2) ||
	    !(bh = ext3cow_bread (NULL, inode, 0, 0, &err))) {
		if (err)
			ext3cow_error(inode->i_sb, __func__,
				   "error %d reading directory #%lu offset 0",
				   err, inode->i_ino);
		else
			ext3cow_warning(inode->i_sb, __func__,
				     "bad directory (dir #%lu) - no data block",
				     inode->i_ino);
		return 1;
	}
	de = (struct ext3cow_dir_entry_2 *) bh->b_data;
	de1 = ext3cow_next_entry(de);
	if (le32_to_cpu(de->inode) != inode->i_ino ||
			!le32_to_cpu(de1->inode) ||
			strcmp (".", de->name) ||
			strcmp ("..", de1->name)) {
		ext3cow_warning (inode->i_sb, "empty_dir",
			      "bad directory (dir #%lu) - no `.' or `..'",
			      inode->i_ino);
		brelse (bh);
		return 1;
	}
	offset = ext3cow_rec_len_from_disk(de->rec_len) +
			ext3cow_rec_len_from_disk(de1->rec_len);
	de = ext3cow_next_entry(de1);
	while (offset < inode->i_size ) {
		if (!bh ||
			(void *) de >= (void *) (bh->b_data+sb->s_blocksize)) {
			err = 0;
			brelse (bh);
			bh = ext3cow_bread (NULL, inode,
				offset >> EXT3COW_BLOCK_SIZE_BITS(sb), 0, &err);
			if (!bh) {
				if (err)
					ext3cow_error(sb, __func__,
						   "error %d reading directory"
						   " #%lu offset %lu",
						   err, inode->i_ino, offset);
				offset += sb->s_blocksize;
				continue;
			}
			de = (struct ext3cow_dir_entry_2 *) bh->b_data;
		}
		if (!ext3cow_check_dir_entry("empty_dir", inode, de, bh, offset)) {
			de = (struct ext3cow_dir_entry_2 *)(bh->b_data +
							 sb->s_blocksize);
			offset = (offset | (sb->s_blocksize - 1)) + 1;
			continue;
		}
		    
		/* Can remove a dir only if all dirents are out of scope -znjp */
		if (le32_to_cpu(de->inode) &&
		    EXT3COW_IS_DIRENT_SCOPED(de, EXT3COW_I_EPOCHNUMBER(inode))) {
			brelse (bh);
			return 0;
		}
		offset += ext3cow_rec_len_from_disk(de->rec_len);
		de = ext3cow_next_entry(de);
	}
	brelse (bh);
	return 1;
}

/* ext3cow_orphan_add() links an unlinked or truncated inode into a list of
 * such inodes, starting at the superblock, in case we crash before the
 * file is closed/deleted, or in case the inode truncate spans multiple
 * transactions and the last transaction is not recovered after a crash.
 *
 * At filesystem recovery time, we walk this list deleting unlinked
 * inodes and truncating linked inodes in ext3cow_orphan_cleanup().
 */
int ext3cow_orphan_add(handle_t *handle, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext3cow_iloc iloc;
	int err = 0, rc;

	mutex_lock(&EXT3COW_SB(sb)->s_orphan_lock);
	if (!list_empty(&EXT3COW_I(inode)->i_orphan))
		goto out_unlock;

	/* Orphan handling is only valid for files with data blocks
	 * being truncated, or files being unlinked. */

	/* @@@ FIXME: Observation from aviro:
	 * I think I can trigger J_ASSERT in ext3cow_orphan_add().  We block
	 * here (on s_orphan_lock), so race with ext3cow_link() which might bump
	 * ->i_nlink. For, say it, character device. Not a regular file,
	 * not a directory, not a symlink and ->i_nlink > 0.
	 *
	 * tytso, 4/25/2009: I'm not sure how that could happen;
	 * shouldn't the fs core protect us from these sort of
	 * unlink()/link() races?
	 */
	J_ASSERT ((S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
		S_ISLNK(inode->i_mode)) || inode->i_nlink == 0);

	BUFFER_TRACE(EXT3COW_SB(sb)->s_sbh, "get_write_access");
	err = ext3cow_journal_get_write_access(handle, EXT3COW_SB(sb)->s_sbh);
	if (err)
		goto out_unlock;

	err = ext3cow_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto out_unlock;

	/* Insert this inode at the head of the on-disk orphan list... */
	NEXT_ORPHAN(inode) = le32_to_cpu(EXT3COW_SB(sb)->s_es->s_last_orphan);
	EXT3COW_SB(sb)->s_es->s_last_orphan = cpu_to_le32(inode->i_ino);
	err = ext3cow_journal_dirty_metadata(handle, EXT3COW_SB(sb)->s_sbh);
	rc = ext3cow_mark_iloc_dirty(handle, inode, &iloc);
	if (!err)
		err = rc;

	/* Only add to the head of the in-memory list if all the
	 * previous operations succeeded.  If the orphan_add is going to
	 * fail (possibly taking the journal offline), we can't risk
	 * leaving the inode on the orphan list: stray orphan-list
	 * entries can cause panics at unmount time.
	 *
	 * This is safe: on error we're going to ignore the orphan list
	 * anyway on the next recovery. */
	if (!err)
		list_add(&EXT3COW_I(inode)->i_orphan, &EXT3COW_SB(sb)->s_orphan);

	jbd_debug(4, "superblock will point to %lu\n", inode->i_ino);
	jbd_debug(4, "orphan inode %lu will point to %d\n",
			inode->i_ino, NEXT_ORPHAN(inode));
out_unlock:
	mutex_unlock(&EXT3COW_SB(sb)->s_orphan_lock);
	ext3cow_std_error(inode->i_sb, err);
	return err;
}

/*
 * ext3cow_orphan_del() removes an unlinked or truncated inode from the list
 * of such inodes stored on disk, because it is finally being cleaned up.
 */
int ext3cow_orphan_del(handle_t *handle, struct inode *inode)
{
	struct list_head *prev;
	struct ext3cow_inode_info *ei = EXT3COW_I(inode);
	struct ext3cow_sb_info *sbi;
	unsigned long ino_next;
	struct ext3cow_iloc iloc;
	int err = 0;

	mutex_lock(&EXT3COW_SB(inode->i_sb)->s_orphan_lock);
	if (list_empty(&ei->i_orphan))
		goto out;

	ino_next = NEXT_ORPHAN(inode);
	prev = ei->i_orphan.prev;
	sbi = EXT3COW_SB(inode->i_sb);

	jbd_debug(4, "remove inode %lu from orphan list\n", inode->i_ino);

	list_del_init(&ei->i_orphan);

	/* If we're on an error path, we may not have a valid
	 * transaction handle with which to update the orphan list on
	 * disk, but we still need to remove the inode from the linked
	 * list in memory. */
	if (!handle)
		goto out;

	err = ext3cow_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto out_err;

	if (prev == &sbi->s_orphan) {
		jbd_debug(4, "superblock will point to %lu\n", ino_next);
		BUFFER_TRACE(sbi->s_sbh, "get_write_access");
		err = ext3cow_journal_get_write_access(handle, sbi->s_sbh);
		if (err)
			goto out_brelse;
		sbi->s_es->s_last_orphan = cpu_to_le32(ino_next);
		err = ext3cow_journal_dirty_metadata(handle, sbi->s_sbh);
	} else {
		struct ext3cow_iloc iloc2;
		struct inode *i_prev =
			&list_entry(prev, struct ext3cow_inode_info, i_orphan)->vfs_inode;

		jbd_debug(4, "orphan inode %lu will point to %lu\n",
			  i_prev->i_ino, ino_next);
		err = ext3cow_reserve_inode_write(handle, i_prev, &iloc2);
		if (err)
			goto out_brelse;
		NEXT_ORPHAN(i_prev) = ino_next;
		err = ext3cow_mark_iloc_dirty(handle, i_prev, &iloc2);
	}
	if (err)
		goto out_brelse;
	NEXT_ORPHAN(inode) = 0;
	err = ext3cow_mark_iloc_dirty(handle, inode, &iloc);

out_err:
	ext3cow_std_error(inode->i_sb, err);
out:
	mutex_unlock(&EXT3COW_SB(inode->i_sb)->s_orphan_lock);
	return err;

out_brelse:
	brelse(iloc.bh);
	goto out_err;
}

static int ext3cow_rmdir (struct inode * dir, struct dentry *dentry)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct ext3cow_dir_entry_2 * de;
	handle_t *handle;

	/* Initialize quotas before so that eventual writes go in
	 * separate transaction */
	dquot_initialize(dir);
	dquot_initialize(dentry->d_inode);

	handle = ext3cow_journal_start(dir, EXT3COW_DELETE_TRANS_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	retval = -ENOENT;
	bh = ext3cow_find_entry(dir, &dentry->d_name, &de);
	if (!bh)
		goto end_rmdir;

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	inode = dentry->d_inode;
	/* Can't rmdir in the past -znjp */
	retval = -EROFS;
	if(is_unchangeable(inode, dentry))
	goto end_rmdir;

	retval = -EIO;
	if (le32_to_cpu(de->inode) != inode->i_ino)
		goto end_rmdir;

	retval = -ENOTEMPTY;
	if (!empty_dir (inode))
		goto end_rmdir;

	retval = ext3cow_delete_entry(handle, dir, de, bh, dentry);
	if (retval)
		goto end_rmdir;
	if (inode->i_nlink != 2)
		ext3cow_warning (inode->i_sb, "ext3cow_rmdir",
			      "empty directory has nlink!=2 (%d)",
			      inode->i_nlink);
	inode->i_version++;
	
  /* We only delete things that were created in the same epoch -znjp */
  if(de->birth_epoch == de->death_epoch){
    clear_nlink(inode);
    /* There's no need to set i_disksize: the fact that i_nlink is
     * zero will ensure that the right thing happens during any
     * recovery. */
    inode->i_size = 0;
    ext3cow_orphan_add(handle, inode);
    drop_nlink(dir);
  }
  EXT3COW_I(inode)->i_flags |= EXT3COW_UNCHANGEABLE_FL;
  inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME_SEC;
  ext3cow_mark_inode_dirty(handle, inode);
  ext3cow_update_dx_flag(dir);
	ext3cow_mark_inode_dirty(handle, dir);

end_rmdir:
	ext3cow_journal_stop(handle);
	brelse (bh);
	return retval;
}

static int ext3cow_unlink(struct inode * dir, struct dentry *dentry)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct ext3cow_dir_entry_2 * de;
	handle_t *handle;

	trace_ext3cow_unlink_enter(dir, dentry);
	/* Initialize quotas before so that eventual writes go
	 * in separate transaction */
	dquot_initialize(dir);
	dquot_initialize(dentry->d_inode);

	handle = ext3cow_journal_start(dir, EXT3COW_DELETE_TRANS_BLOCKS(dir->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	retval = -ENOENT;
	bh = ext3cow_find_entry(dir, &dentry->d_name, &de);
	if (!bh)
		goto end_unlink;

	inode = dentry->d_inode;
  /* Can't unlink in the past -znjp */
  retval = -EROFS;
  if(is_unchangeable(inode, dentry))
    goto end_unlink;  


	retval = -EIO;
	if (le32_to_cpu(de->inode) != inode->i_ino)
		goto end_unlink;

	if (!inode->i_nlink) {
		ext3cow_warning (inode->i_sb, "ext3cow_unlink",
			      "Deleting nonexistent file (%lu), %d",
			      inode->i_ino, inode->i_nlink);
		set_nlink(inode, 1);
	}
	retval = ext3cow_delete_entry(handle, dir, de, bh, dentry);
	if (retval)
		goto end_unlink;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME_SEC;
	ext3cow_update_dx_flag(dir);
	ext3cow_mark_inode_dirty(handle, dir);
  /* If the file should be deleted here, don't actually delete it
   * but mark it unchangeable, i.e. it's now in the past. -znjp */

  /* If file was created in this epoch, then we actually unlink it,
   * if not, then it belongs to the past, so mark it unchangeable -znjp */
  if(de->birth_epoch == de->death_epoch){
       drop_nlink(inode);
    if (!inode->i_nlink){
      ext3cow_orphan_add(handle, inode);
    }
  }else{
    if(!(inode->i_nlink - 1))
      EXT3COW_I(inode)->i_flags |= EXT3COW_UNCHANGEABLE_FL; 
  }
	inode->i_ctime = dir->i_ctime;
	ext3cow_mark_inode_dirty(handle, inode);
	retval = 0;

end_unlink:
	ext3cow_journal_stop(handle);
	brelse (bh);
	trace_ext3cow_unlink_exit(dentry, retval);
	return retval;
}

static int ext3cow_symlink (struct inode * dir,
		struct dentry *dentry, const char * symname)
{
	handle_t *handle;
	struct inode * inode;
	int l, err, retries = 0;
	int credits;

	l = strlen(symname)+1;
	if (l > dir->i_sb->s_blocksize)
		return -ENAMETOOLONG;

	dquot_initialize(dir);

	if (l > EXT3COW_N_BLOCKS * 4) {
		/*
		 * For non-fast symlinks, we just allocate inode and put it on
		 * orphan list in the first transaction => we need bitmap,
		 * group descriptor, sb, inode block, quota blocks, and
		 * possibly selinux xattr blocks.
		 */
		credits = 4 + EXT3COW_MAXQUOTAS_INIT_BLOCKS(dir->i_sb) +
			  EXT3COW_XATTR_TRANS_BLOCKS;
	} else {
		/*
		 * Fast symlink. We have to add entry to directory
		 * (EXT3COW_DATA_TRANS_BLOCKS + EXT3COW_INDEX_EXTRA_TRANS_BLOCKS),
		 * allocate new inode (bitmap, group descriptor, inode block,
		 * quota blocks, sb is already counted in previous macros).
		 */
		credits = EXT3COW_DATA_TRANS_BLOCKS(dir->i_sb) +
			  EXT3COW_INDEX_EXTRA_TRANS_BLOCKS + 3 +
			  EXT3COW_MAXQUOTAS_INIT_BLOCKS(dir->i_sb);
	}
retry:
	handle = ext3cow_journal_start(dir, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	inode = ext3cow_new_inode (handle, dir, &dentry->d_name, S_IFLNK|S_IRWXUGO);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_stop;

	if (l > EXT3COW_N_BLOCKS * 4) {
		inode->i_op = &ext3cow_symlink_inode_operations;
		ext3cow_set_aops(inode);
		/*
		 * We cannot call page_symlink() with transaction started
		 * because it calls into ext3cow_write_begin() which acquires page
		 * lock which ranks below transaction start (and it can also
		 * wait for journal commit if we are running out of space). So
		 * we have to stop transaction now and restart it when symlink
		 * contents is written. 
		 *
		 * To keep fs consistent in case of crash, we have to put inode
		 * to orphan list in the mean time.
		 */
		drop_nlink(inode);
		err = ext3cow_orphan_add(handle, inode);
		ext3cow_journal_stop(handle);
		if (err)
			goto err_drop_inode;
		err = __page_symlink(inode, symname, l, 1);
		if (err)
			goto err_drop_inode;
		/*
		 * Now inode is being linked into dir (EXT3COW_DATA_TRANS_BLOCKS
		 * + EXT3COW_INDEX_EXTRA_TRANS_BLOCKS), inode is also modified
		 */
		handle = ext3cow_journal_start(dir,
				EXT3COW_DATA_TRANS_BLOCKS(dir->i_sb) +
				EXT3COW_INDEX_EXTRA_TRANS_BLOCKS + 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto err_drop_inode;
		}
		set_nlink(inode, 1);
		err = ext3cow_orphan_del(handle, inode);
		if (err) {
			ext3cow_journal_stop(handle);
			drop_nlink(inode);
			goto err_drop_inode;
		}
	} else {
		inode->i_op = &ext3cow_fast_symlink_inode_operations;
		memcpy((char*)&EXT3COW_I(inode)->i_data,symname,l);
		inode->i_size = l-1;
	}
	EXT3COW_I(inode)->i_disksize = inode->i_size;
	err = ext3cow_add_nondir(handle, dentry, inode);
out_stop:
	ext3cow_journal_stop(handle);
	if (err == -ENOSPC && ext3cow_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
err_drop_inode:
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

static int ext3cow_link (struct dentry * old_dentry,
		struct inode * dir, struct dentry *dentry)
{
	handle_t *handle;
	struct inode *inode = old_dentry->d_inode;
	int err, retries = 0;

	if (inode->i_nlink >= EXT3COW_LINK_MAX)
		return -EMLINK;

	dquot_initialize(dir);

retry:
	handle = ext3cow_journal_start(dir, EXT3COW_DATA_TRANS_BLOCKS(dir->i_sb) +
					EXT3COW_INDEX_EXTRA_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(dir))
		handle->h_sync = 1;

	inode->i_ctime = CURRENT_TIME_SEC;
	inc_nlink(inode);
	ihold(inode);

	err = ext3cow_add_entry(handle, dentry, inode);
	if (!err) {
		ext3cow_mark_inode_dirty(handle, inode);
		d_instantiate(dentry, inode);
	} else {
		drop_nlink(inode);
		iput(inode);
	}
	ext3cow_journal_stop(handle);
	if (err == -ENOSPC && ext3cow_should_retry_alloc(dir->i_sb, &retries))
		goto retry;
	return err;
}

#define PARENT_INO(buffer) \
	(ext3cow_next_entry((struct ext3cow_dir_entry_2 *)(buffer))->inode)

/*
 * Anybody can rename anything with this: the permission checks are left to the
 * higher-level routines.
 */
static int ext3cow_rename (struct inode * old_dir, struct dentry *old_dentry,
			   struct inode * new_dir,struct dentry *new_dentry)
{
	handle_t *handle;
	struct inode * old_inode, * new_inode;
	struct buffer_head * old_bh, * new_bh, * dir_bh;
	struct ext3cow_dir_entry_2 * old_de, * new_de;
	int retval, flush_file = 0;

	dquot_initialize(old_dir);
	dquot_initialize(new_dir);

	old_bh = new_bh = dir_bh = NULL;

	/* Initialize quotas before so that eventual writes go
	 * in separate transaction */
	if (new_dentry->d_inode)
		dquot_initialize(new_dentry->d_inode);
	handle = ext3cow_journal_start(old_dir, 2 *
					EXT3COW_DATA_TRANS_BLOCKS(old_dir->i_sb) +
					EXT3COW_INDEX_EXTRA_TRANS_BLOCKS + 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (IS_DIRSYNC(old_dir) || IS_DIRSYNC(new_dir))
		handle->h_sync = 1;

	old_bh = ext3cow_find_entry(old_dir, &old_dentry->d_name, &old_de);
	/*
	 *  Check for inode number is _not_ due to possible IO errors.
	 *  We might rmdir the source, keep it as pwd of some process
	 *  and merrily kill the link to whatever was created under the
	 *  same name. Goodbye sticky bit ;-<
	 */
	old_inode = old_dentry->d_inode;
	retval = -ENOENT;
	if (!old_bh || le32_to_cpu(old_de->inode) != old_inode->i_ino)
		goto end_rename;

	new_inode = new_dentry->d_inode;
	new_bh = ext3cow_find_entry(new_dir, &new_dentry->d_name, &new_de);
	if (new_bh) {
		if (!new_inode) {
			brelse (new_bh);
			new_bh = NULL;
		}
	}
	/* can't move something into the past -znjp */
  retval = -EROFS;
  if(is_unchangeable(new_inode, new_dentry)) 
    goto end_rename;
  /* can't some move from the past -znjp */
  if(is_unchangeable(old_inode, old_dentry))
    goto end_rename;
	if (S_ISDIR(old_inode->i_mode)) {
		if (new_inode) {
			retval = -ENOTEMPTY;
			if (!empty_dir (new_inode))
				goto end_rename;
		}
		retval = -EIO;
		dir_bh = ext3cow_bread (handle, old_inode, 0, 0, &retval);
		if (!dir_bh)
			goto end_rename;
		if (le32_to_cpu(PARENT_INO(dir_bh->b_data)) != old_dir->i_ino)
			goto end_rename;
		retval = -EMLINK;
		if (!new_inode && new_dir!=old_dir &&
				new_dir->i_nlink >= EXT3COW_LINK_MAX)
			goto end_rename;
	}
	if (!new_bh) {
		retval = ext3cow_add_entry (handle, new_dentry, old_inode);
		if (retval)
			goto end_rename;
	} else {
		BUFFER_TRACE(new_bh, "get write access");
		retval = ext3cow_journal_get_write_access(handle, new_bh);
		if (retval)
			goto journal_error;
		new_de->inode = cpu_to_le32(old_inode->i_ino);
		if (EXT3COW_HAS_INCOMPAT_FEATURE(new_dir->i_sb,
					      EXT3COW_FEATURE_INCOMPAT_FILETYPE))
			new_de->file_type = old_de->file_type;
		new_dir->i_version++;
		new_dir->i_ctime = new_dir->i_mtime = CURRENT_TIME_SEC;
		ext3cow_mark_inode_dirty(handle, new_dir);
		BUFFER_TRACE(new_bh, "call ext3cow_journal_dirty_metadata");
		retval = ext3cow_journal_dirty_metadata(handle, new_bh);
		if (retval)
			goto journal_error;
		brelse(new_bh);
		new_bh = NULL;
	}

	/*
	 * Like most other Unix systems, set the ctime for inodes on a
	 * rename.
	 */
	old_inode->i_ctime = CURRENT_TIME_SEC;
	ext3cow_mark_inode_dirty(handle, old_inode);

	/*
	 * ok, that's it
	 */
	if (le32_to_cpu(old_de->inode) != old_inode->i_ino ||
	    old_de->name_len != old_dentry->d_name.len ||
	    strncmp(old_de->name, old_dentry->d_name.name, old_de->name_len) ||
	    (retval = ext3cow_delete_entry(handle, old_dir,
					 old_de, old_bh, new_dentry)) == -ENOENT) {
		/* old_de could have moved from under us during htree split, so
		 * make sure that we are deleting the right entry.  We might
		 * also be pointing to a stale entry in the unused part of
		 * old_bh so just checking inum and the name isn't enough. */
		struct buffer_head *old_bh2;
		struct ext3cow_dir_entry_2 *old_de2;

		old_bh2 = ext3cow_find_entry(old_dir, &old_dentry->d_name,
					  &old_de2);
		if (old_bh2) {
			retval = ext3cow_delete_entry(handle, old_dir,
						   old_de2, old_bh2, new_dentry);
			brelse(old_bh2);
		}
	}
	if (retval) {
		ext3cow_warning(old_dir->i_sb, "ext3cow_rename",
				"Deleting old file (%lu), %d, error=%d",
				old_dir->i_ino, old_dir->i_nlink, retval);
	}

	if (new_inode) {
		new_inode->i_ctime = CURRENT_TIME_SEC;
	}
	if(!is_unchangeable(old_inode, old_dentry))
	    old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME_SEC;
	ext3cow_update_dx_flag(old_dir);
	if (dir_bh) {
		BUFFER_TRACE(dir_bh, "get_write_access");
		retval = ext3cow_journal_get_write_access(handle, dir_bh);
		if (retval)
			goto journal_error;
		PARENT_INO(dir_bh->b_data) = cpu_to_le32(new_dir->i_ino);
		BUFFER_TRACE(dir_bh, "call ext3cow_journal_dirty_metadata");
		retval = ext3cow_journal_dirty_metadata(handle, dir_bh);
		if (retval) {
journal_error:
			ext3cow_std_error(new_dir->i_sb, retval);
			goto end_rename;
		}
		if (!new_inode) {
			inc_nlink(new_dir);
			ext3cow_update_dx_flag(new_dir);
			ext3cow_mark_inode_dirty(handle, new_dir);
		}
	}
	ext3cow_mark_inode_dirty(handle, old_dir);
	if (new_inode) {
		ext3cow_mark_inode_dirty(handle, new_inode);
		if (!new_inode->i_nlink)
			ext3cow_orphan_add(handle, new_inode);
		if (ext3cow_should_writeback_data(new_inode))
			flush_file = 1;
	}
	retval = 0;

end_rename:
	brelse (dir_bh);
	brelse (old_bh);
	brelse (new_bh);
	ext3cow_journal_stop(handle);
	if (retval == 0 && flush_file)
		filemap_flush(old_inode->i_mapping);
	return retval;
}

/* ext3cow_fake_inode: This function creates a VFS-only inode
 * used for properly scoping views into the past file system - znjp
 */
struct inode *ext3cow_fake_inode(struct inode *inode,
                                 unsigned int epoch_number)
{
  struct inode * fake_inode = NULL;
  struct ext3cow_inode_info * ini = NULL;
  struct ext3cow_inode_info * fake_ini = NULL;
  static unsigned int last_ino = UINT_MAX;
  int err = 0;
  int block = -1;

  if(NULL == inode){
    printk(KERN_ERR "Trying to duplicate a NULL inode.\n");
    return NULL;
  }

  if(EXT3COW_IS_FAKEINODE(inode)){
    printk(KERN_ERR "Trying to fake a fake inode.\n");
    return inode;
  }

  printk(KERN_INFO "** faking inode %lu\n", inode->i_ino);

  ini = EXT3COW_I(inode);
  
  /* Create a new VFS-only inode */
  fake_inode = new_inode(inode->i_sb);   
  err = PTR_ERR(fake_inode);
  if(!IS_ERR(fake_inode)){

    fake_ini = EXT3COW_I(fake_inode);

    printk(KERN_INFO "** got inode %lu setting with %u\n", fake_inode->i_ino,
           last_ino);

    /* When inode is a directory, we can fake the inode number */
    //if(S_ISDIR(inode->i_mode))
    fake_inode->i_ino  = --last_ino;

    fake_inode->i_mode = inode->i_mode;

    fake_inode->i_uid  = inode->i_uid;
    fake_inode->i_gid  = inode->i_gid;

    atomic_set(&fake_inode->i_count, 1);

    set_nlink(fake_inode, inode->i_nlink);
    fake_inode->i_size          = inode->i_size;
    fake_inode->i_atime.tv_sec  = inode->i_atime.tv_sec;
    fake_inode->i_ctime.tv_sec  = inode->i_ctime.tv_sec;
    fake_inode->i_mtime.tv_sec  = inode->i_mtime.tv_sec;
    fake_inode->i_atime.tv_nsec = inode->i_atime.tv_nsec;
    fake_inode->i_ctime.tv_nsec = inode->i_ctime.tv_nsec;
    fake_inode->i_mtime.tv_nsec = inode->i_mtime.tv_nsec;

    fake_ini->i_state_flags = ini->i_state_flags;
    fake_ini->i_dir_start_lookup = ini->i_dir_start_lookup;
    fake_ini->i_dtime = ini->i_dtime;

    fake_inode->i_blocks  = inode->i_blocks;
    fake_ini->i_flags     = ini->i_flags;
#ifdef EXT3COW_FRAGMENTS
    /* Taken out for versioning -znjp */
    //fake_ini->i_faddr     = ini->i_faddr;
    //fake_ini->i_frag_no   = ini->i_frag_no;
    //fake_ini->i_frag_size = ini->i_frag_size;
#endif
    fake_ini->i_file_acl = ini->i_file_acl;
    if (!S_ISREG(fake_inode->i_mode)) {
      fake_ini->i_dir_acl = ini->i_dir_acl;
    } 
    fake_ini->i_disksize = inode->i_size;
    fake_inode->i_generation = inode->i_generation;
    //TODO: This could be wrong.
    //fake_ini->i_block_group = ini->i_block_group; //iloc.block_group;

    for (block = 0; block < EXT3COW_N_BLOCKS; block++)
      fake_ini->i_data[block] = ini->i_data[block];

    fake_ini->i_extra_isize = ini->i_extra_isize;

    /* set copy-on-write bitmap to 0 */
    fake_ini->i_cow_bitmap = 0x0000;
    
    /* Mark fake inode unchangeable, etc. */
    fake_ini->i_flags |= EXT3COW_UNCHANGEABLE_FL;
    fake_ini->i_flags |= EXT3COW_UNVERSIONABLE_FL;
    fake_ini->i_flags |= EXT3COW_FAKEINODE_FL;
    fake_ini->i_flags |= EXT3COW_IMMUTABLE_FL;

    /* Make sure we get the right operations */
    if (S_ISREG(fake_inode->i_mode)) {
      fake_inode->i_op = &ext3cow_file_inode_operations;
      fake_inode->i_fop = &ext3cow_file_operations;
      ext3cow_set_aops(fake_inode);
    } else if (S_ISDIR(fake_inode->i_mode)) {
      fake_inode->i_op = &ext3cow_dir_inode_operations;
      fake_inode->i_fop = &ext3cow_dir_operations;
    } else if (S_ISLNK(fake_inode->i_mode)) {
      //if (ext3cow_inode_is_fast_symlink(cow_inode))
      if((S_ISLNK(fake_inode->i_mode) && fake_inode->i_blocks - 
          (EXT3COW_I(fake_inode)->i_file_acl ? 
           (fake_inode->i_sb->s_blocksize >> 9) : 0)))
        fake_inode->i_op = &ext3cow_fast_symlink_inode_operations;
      else {
        fake_inode->i_op = &ext3cow_symlink_inode_operations;
        ext3cow_set_aops(fake_inode);
      }
    } else {
      fake_inode->i_op = &ext3cow_special_inode_operations;
    }

    fake_ini->i_epoch_number = epoch_number;
    fake_ini->i_next_inode = 0;
    
    iput(inode); /* dec i_count */

    return fake_inode;
  }else
    ext3cow_warning(inode->i_sb, "ext3cow_fake_inode",
                    "Could not create fake inode.");
   
       return NULL;
}

/* ext3cow_dup_inode: This function creates a new inode, 
* copies all the metadata from the passed in inode,  
* and adds it to the version chain, creating a new version.  
 * The head of the chain never changes; it is always the most current version.
 * Similar in nature to ext3cow_creat and ext3cow_read_inode. -znjp
 */
int ext3cow_dup_inode(struct inode *dir, struct inode *inode){

  struct inode *cow_inode = NULL;
  struct inode *parent = NULL;
  struct ext3cow_inode_info *ini = NULL;
  struct ext3cow_inode_info *cow_ini = NULL;
  handle_t *handle = NULL;
  int err = 0;
  int block = -1;
  unsigned int epoch_number_temp = 0;
  int retries = 0;

  printk(KERN_INFO "** duping inode %lu\n", inode->i_ino);

  if(EXT3COW_IS_UNVERSIONABLE(inode))
    return 0;

  if(NULL == inode){
    printk(KERN_ERR "Trying to duplicate a NULL inode.\n");
    return -1;
  }

       if (inode->i_nlink == 0) {
               if (inode->i_mode == 0 ||
                   !(EXT3COW_SB(inode->i_sb)->s_mount_state & EXT3COW_ORPHAN_FS)) {
                       /* this inode is deleted */
      return -1;
               }
               /* The only unlinked inodes we let through here have
                * valid i_mode and are being read by the orphan
                * recovery code: that's fine, we're about to complete
                * the process of deleting those. */
       }

  ini = EXT3COW_I(inode);

  /* This is for truncate, which can't pass in a parent */
  if(NULL == dir)
    parent = inode;
  else
    parent = dir;

retry:
       handle = ext3cow_journal_start(parent, EXT3COW_DATA_TRANS_BLOCKS(dir->i_sb) +
                                       EXT3COW_INDEX_EXTRA_TRANS_BLOCKS + 3 +
                                       2*EXT3COW_QUOTA_INIT_BLOCKS(dir->i_sb));
       if (IS_ERR(handle))
               return PTR_ERR(handle);

       if (IS_DIRSYNC(parent))
               handle->h_sync = 1;

       cow_inode = ext3cow_new_inode (handle, parent, &(get_dentry_for_inode(inode)->d_name), inode->i_mode);
       err = PTR_ERR(cow_inode);
       if (!IS_ERR(cow_inode)) {

    printk(KERN_INFO "  ** Allocated new inode %lu\n", cow_inode->i_ino);

    cow_ini = EXT3COW_I(cow_inode);
    
    cow_inode->i_mode = inode->i_mode;
    cow_inode->i_uid  = inode->i_uid;
    cow_inode->i_gid  = inode->i_gid;

    set_nlink(cow_inode, inode->i_nlink);
    cow_inode->i_size          = inode->i_size;
    cow_inode->i_atime.tv_sec  = inode->i_atime.tv_sec;
    cow_inode->i_ctime.tv_sec  = inode->i_ctime.tv_sec;
    cow_inode->i_mtime.tv_sec  = inode->i_mtime.tv_sec;
    cow_inode->i_atime.tv_nsec = inode->i_atime.tv_nsec;
    cow_inode->i_ctime.tv_nsec = inode->i_ctime.tv_nsec;
    cow_inode->i_mtime.tv_nsec = inode->i_mtime.tv_nsec;

    cow_ini->i_state_flags = ini->i_state_flags;
    cow_ini->i_dir_start_lookup = ini->i_dir_start_lookup;
    cow_ini->i_dtime = ini->i_dtime;

    cow_inode->i_blocks  = inode->i_blocks;
    cow_ini->i_flags     = ini->i_flags;
#ifdef EXT3COW_FRAGMENTS
    /* Taken out for versioning -znjp */
    //cow_ini->i_faddr     = ini->i_faddr;
    //cow_ini->i_frag_no   = ini->i_frag_no;
    //cow_ini->i_frag_size = ini->i_frag_size;
#endif
    cow_ini->i_file_acl = ini->i_file_acl;
    if (!S_ISREG(cow_inode->i_mode)) {
      cow_ini->i_dir_acl = ini->i_dir_acl;
    } 
    cow_ini->i_disksize = inode->i_size;
    cow_inode->i_generation = inode->i_generation;
    //TODO: This could be wrong.
    cow_ini->i_block_group = ini->i_block_group; //iloc.block_group;

    for (block = 0; block < EXT3COW_N_BLOCKS; block++)
      cow_ini->i_data[block] = ini->i_data[block];

    //TODO: This could be wrong
    //cow_ini->i_orphan = NULL; //INIT_LIST_HEAD(&ei->i_orphan);
   
    cow_ini->i_extra_isize = ini->i_extra_isize;

    /* Make sure we get the right operations */
    if (S_ISREG(cow_inode->i_mode)) {
      cow_inode->i_op = &ext3cow_file_inode_operations;
      cow_inode->i_fop = &ext3cow_file_operations;
      ext3cow_set_aops(cow_inode);
    } else if (S_ISDIR(cow_inode->i_mode)) {
      cow_inode->i_op = &ext3cow_dir_inode_operations;
      cow_inode->i_fop = &ext3cow_dir_operations;
    } else if (S_ISLNK(cow_inode->i_mode)) {
      //if (ext3cow_inode_is_fast_symlink(cow_inode))
      if((S_ISLNK(cow_inode->i_mode) && cow_inode->i_blocks - 
          (EXT3COW_I(cow_inode)->i_file_acl ? 
           (cow_inode->i_sb->s_blocksize >> 9) : 0)))
        cow_inode->i_op = &ext3cow_fast_symlink_inode_operations;
      else {
        cow_inode->i_op = &ext3cow_symlink_inode_operations;
        ext3cow_set_aops(cow_inode);
      }
    } else {
      cow_inode->i_op = &ext3cow_special_inode_operations;
      /*
      if (raw_inode->i_block[0])
        init_special_inode(inode, inode->i_mode,
                           old_decode_dev(le32_to_cpu(raw_inode->i_block[0])));
      else
        init_special_inode(inode, inode->i_mode,
                           new_decode_dev(le32_to_cpu(raw_inode->i_block[1])));
      */
    }
    /* Dup in the direct cow bitmap */
    cow_ini->i_cow_bitmap = ini->i_cow_bitmap;
    ini->i_cow_bitmap     = 0x0000;
    /* Mark new inode unchangeable */
    cow_ini->i_flags |= EXT3COW_UNCHANGEABLE_FL;
    /* Switch epoch numbers */
    epoch_number_temp = ini->i_epoch_number;
    ini->i_epoch_number = cow_ini->i_epoch_number;
    cow_ini->i_epoch_number = epoch_number_temp;
    /* Chain Inodes together */
    cow_ini->i_next_inode = ini->i_next_inode;
    ini->i_next_inode = cow_inode->i_ino;

    ext3cow_mark_inode_dirty(handle, cow_inode);
    ext3cow_mark_inode_dirty(handle, inode);
    
    iput(cow_inode); /* dec i_count */

    err = 0;
       }
       ext3cow_journal_stop(handle);
       if (err == -ENOSPC && ext3cow_should_retry_alloc(dir->i_sb, &retries))
               goto retry;
       return err;

}

/* ext3cow_reclaim_dup_inode: rolls back a recently dup'd inode
 * on error, including epoch number and bitmaps.  Should not
 * be used for removing versions.  */
int ext3cow_reclaim_dup_inode(struct inode *dir, struct inode *inode)
{
  handle_t *handle = NULL;
  int err = 0;
  struct inode *old_inode = NULL;
  struct inode *parent = dir;

  if(!parent)
    parent = inode;

  if(is_bad_inode(inode))
    return -1;
  
  handle = ext3cow_journal_start(parent, 
                                 EXT3COW_DELETE_TRANS_BLOCKS(parent->i_sb));
  if(IS_ERR(handle))
    return PTR_ERR(handle);

  if(IS_DIRSYNC(parent))
    handle->h_sync = 1;

  old_inode = ext3cow_iget(parent->i_sb, EXT3COW_I_NEXT_INODE(inode));
  err = PTR_ERR(old_inode);
  if (!IS_ERR(old_inode)){

    EXT3COW_I(inode)->i_epoch_number = EXT3COW_I_EPOCHNUMBER(old_inode);
    EXT3COW_I(inode)->i_cow_bitmap   = EXT3COW_I(old_inode)->i_cow_bitmap;
    EXT3COW_I(inode)->i_next_inode   = EXT3COW_I(old_inode)->i_next_inode;
    set_nlink(old_inode, 0);

    iput(old_inode);
    ext3cow_mark_inode_dirty(handle, inode);
  }else
    ext3cow_error(inode->i_sb, "ext3cow_reclaim_dup_inode", 
                  "Couldn't remove dup'd inode.");
  
  ext3cow_journal_stop(handle);
  
  return 0;
}

/*
 * directories can handle most operations...
 */
const struct inode_operations ext3cow_dir_inode_operations = {
	.create		= ext3cow_create,
	.lookup		= ext3cow_lookup,
	.link		= ext3cow_link,
	.unlink		= ext3cow_unlink,
	.symlink	= ext3cow_symlink,
	.mkdir		= ext3cow_mkdir,
	.rmdir		= ext3cow_rmdir,
	.mknod		= ext3cow_mknod,
	.rename		= ext3cow_rename,
	.setattr	= ext3cow_setattr,
#ifdef CONFIG_EXT3COW_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext3cow_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.get_acl	= ext3cow_get_acl,
};

const struct inode_operations ext3cow_special_inode_operations = {
	.setattr	= ext3cow_setattr,
#ifdef CONFIG_EXT3COW_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext3cow_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.get_acl	= ext3cow_get_acl,
};
