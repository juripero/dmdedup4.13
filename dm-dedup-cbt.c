/*
 * Copyright (C) 2012-2018 Vasily Tarasov
 * Copyright (C) 2012-2014 Geoff Kuenning
 * Copyright (C) 2012-2014 Sonam Mandal
 * Copyright (C) 2012-2014 Karthikeyani Palanisami
 * Copyright (C) 2012-2014 Philip Shilane
 * Copyright (C) 2012-2014 Sagar Trehan
 * Copyright (C) 2012-2018 Erez Zadok
 * Copyright (c) 2016-2017 Vinothkumar Raja
 * Copyright (c) 2017-2017 Nidhi Panpalia
 * Copyright (c) 2017-2018 Noopur Maheshwari
 * Copyright (c) 2018-2018 Rahul Rane
 * Copyright (c) 2012-2018 Stony Brook University
 * Copyright (c) 2012-2018 The Research Foundation for SUNY
 * This file is released under the GPL.
 */

#include <linux/errno.h>
#include "persistent-data/dm-btree.h"
#include "persistent-data/dm-space-map.h"
#include "persistent-data/dm-space-map-disk.h"
#include "persistent-data/dm-block-manager.h"
#include "persistent-data/dm-transaction-manager.h"

#include "dm-dedup-cbt.h"
#include "dm-dedup-backend.h"
#include "dm-dedup-kvstore.h"

#define DELETED_ENTRY 0x6B

#define UINT32_MAX	(4294967295U)

#define METADATA_BSIZE 4096
#define METADATA_MAXLOCKS 5
#define METADATA_SUPERBLOCK_LOCATION 0
#define PRIVATE_DATA_SIZE 16 /* physical and logical block counter */
#define DM_DEDUP_MAGIC 0x44447570 /* Hex value for "DDUP" */
#define DM_DEDUP_VERSION 1
#define SUPERBLOCK_CSUM_XOR 189575
struct metadata {
	struct dm_block_manager *meta_bm;
	struct dm_transaction_manager *tm;
	struct dm_space_map *data_sm;
	struct dm_space_map *meta_sm;

	/*
	 * XXX: Currently we support only one linear and one sparse KVS.
	 */
	struct kvstore_cbt_linear *kvs_linear;
	struct kvstore_cbt_sparse *kvs_sparse;

	u8 private_data[PRIVATE_DATA_SIZE];

};

struct kvstore_cbt_linear {
	struct kvstore ckvs;

	struct dm_btree_info info;
	u64 root;
};

struct kvstore_cbt_sparse {
	struct kvstore ckvs;
	u32 entry_size;

	struct dm_btree_info info;
	u64 root;

    /* We will put max limit for linear probing. We are maintaining two
     * values for that. First one indicates current max value for linear
     * probing and second is hard limit until which linear probing is
     * allowed.
     */
	u32 curr_lpmax;
	u32 lpmax;

};

enum superblock_flags {
	CLEAN_SHUTDOWN /* on disk flag to mark clean shutdown */
};

#define SPACE_MAP_ROOT_SIZE 128

struct metadata_superblock {
	__le32 csum; /* Checksum of superblock except for this field. */
	__le64 magic; /* Magic number to check against */
	__le32 version; /* Metadata root version */
	__le32 flags; /* General purpose flags */
	__le64 blocknr;	/* This block number, dm_block_t. */
	__u8 uuid[16]; /* UUID of device (Not used) */
	__u8 last_curr_lpmax; /*Stores current limit on linear probing. */
	/* Metadata space map */
	__u8 metadata_space_map_root[SPACE_MAP_ROOT_SIZE];
	__u8 data_space_map_root[SPACE_MAP_ROOT_SIZE]; /* Data space map */
	__le64 lbn_pbn_root; /* lbn pbn btree root. */
	__le64 hash_pbn_root; /* hash pbn btree root. */
	__le32 data_block_size;	/* In bytes */
	__le32 metadata_block_size; /* In bytes */
	__u8 private_data[PRIVATE_DATA_SIZE]; /* Dmdedup counters */
	__le64 metadata_nr_blocks;/* Number of metadata blocks used. */
} __packed;

static int __begin_transaction(struct metadata *md)
{
	int r;
	struct metadata_superblock *disk_super;
	struct dm_block *sblock;

	r = dm_bm_read_lock(md->meta_bm, METADATA_SUPERBLOCK_LOCATION,
			    NULL, &sblock);
	if (r)
		return r;

	disk_super = dm_block_data(sblock);

	if (md->kvs_linear)
		md->kvs_linear->root = le64_to_cpu(disk_super->lbn_pbn_root);

	if (md->kvs_sparse) {
		md->kvs_sparse->root = le64_to_cpu(disk_super->hash_pbn_root);
		md->kvs_sparse->curr_lpmax = disk_super->last_curr_lpmax;
	}

	memcpy(md->private_data, disk_super->private_data, PRIVATE_DATA_SIZE);

	dm_bm_unlock(sblock);

	return r;
}

static int __commit_transaction(struct metadata *md, bool clean_shutdown_flag)
{
	int r = 0;
	size_t metadata_len, data_len;
	struct metadata_superblock *disk_super;
	struct dm_block *sblock;

	BUILD_BUG_ON(sizeof(struct metadata_superblock) > 512);

	r = dm_sm_commit(md->data_sm);
	if (r < 0)
		goto out;

	r = dm_tm_pre_commit(md->tm);
	if (r < 0)
		goto out;

	r = dm_sm_root_size(md->meta_sm, &metadata_len);
	if (r < 0)
		goto out;

	r = dm_sm_root_size(md->data_sm, &data_len);
	if (r < 0)
		goto out;

	r = dm_bm_write_lock(md->meta_bm, METADATA_SUPERBLOCK_LOCATION,
			     NULL, &sblock);
	if (r)
		goto out;

	disk_super = dm_block_data(sblock);

	/* if destroy flag is set, set the bit 1 otherwise 0 */
	if (clean_shutdown_flag)
		disk_super->flags |= (1 << CLEAN_SHUTDOWN);
	else
		disk_super->flags &= ~(1 << CLEAN_SHUTDOWN);

	if (md->kvs_linear)
		disk_super->lbn_pbn_root = cpu_to_le64(md->kvs_linear->root);

	if (md->kvs_sparse) {
		disk_super->hash_pbn_root = cpu_to_le64(md->kvs_sparse->root);
		disk_super->last_curr_lpmax = md->kvs_sparse->curr_lpmax;
	}

	r = dm_sm_copy_root(md->meta_sm,
			    &disk_super->metadata_space_map_root, metadata_len);

	if (r < 0)
		goto out_locked;

	r = dm_sm_copy_root(md->data_sm, &disk_super->data_space_map_root,
			    data_len);
	if (r < 0)
		goto out_locked;

	memcpy(disk_super->private_data, md->private_data, PRIVATE_DATA_SIZE);

	disk_super->csum = cpu_to_le32(dm_bm_checksum(&disk_super->flags,
					sizeof(struct metadata_superblock)
					- sizeof(__le32),
					SUPERBLOCK_CSUM_XOR));

	r = dm_tm_commit(md->tm, sblock);

	goto out;

out_locked:
	dm_bm_unlock(sblock);

out:
	return r;
}

static int write_initial_superblock(struct metadata *md)
{
	int r;
	size_t meta_len, data_len;
	struct dm_block *sblock;
	struct metadata_superblock *disk_super;

	r = dm_sm_root_size(md->meta_sm, &meta_len);
	if (r < 0)
		return r;

	r = dm_sm_root_size(md->data_sm, &data_len);
	if (r < 0)
		return r;

	r = dm_sm_commit(md->data_sm);
	if (r < 0)
		return r;

	r = dm_tm_pre_commit(md->tm);
	if (r < 0)
		return r;

	r = dm_bm_write_lock_zero(md->meta_bm, METADATA_SUPERBLOCK_LOCATION,
				  NULL, &sblock);
	if (r < 0)
		return r;

	disk_super = dm_block_data(sblock);

	r = dm_sm_copy_root(md->meta_sm, &disk_super->metadata_space_map_root,
			    meta_len);
	if (r < 0)
		goto bad_locked;

	r = dm_sm_copy_root(md->data_sm, &disk_super->data_space_map_root,
			    data_len);
	if (r < 0)
		goto bad_locked;

	disk_super->magic = cpu_to_le32(DM_DEDUP_MAGIC);
	disk_super->version = DM_DEDUP_VERSION;

	disk_super->data_block_size = cpu_to_le32(METADATA_BSIZE);
	disk_super->metadata_block_size = cpu_to_le32(METADATA_BSIZE);

	disk_super->blocknr = cpu_to_le64(dm_block_location(sblock));

	/* set the clean shutdown flag to 0 */
	disk_super->flags &= ~(1 << CLEAN_SHUTDOWN);

	return dm_tm_commit(md->tm, sblock);

bad_locked:
	dm_bm_unlock(sblock);
	return r;
}

static int superblock_all_zeroes(struct dm_block_manager *bm, bool *result)
{
	int r;
	unsigned int i;
	struct dm_block *b;
	__le64 *data_le, zero = cpu_to_le64(0);
	unsigned int sb_block_size = dm_bm_block_size(bm) / sizeof(__le64);

	/*
	 * We can't use a validator here - it may be all zeroes.
	 */
	r = dm_bm_read_lock(bm, METADATA_SUPERBLOCK_LOCATION, NULL, &b);
	if (r)
		return r;

	data_le = dm_block_data(b);
	*result = true;
	for (i = 0; i < sb_block_size; i++) {
		if (data_le[i] != zero) {
			*result = false;
			break;
		}
	}

	dm_bm_unlock(b);
	return 0;
}

static int verify_superblock(struct dm_block_manager *bm)
{
	int r;
	struct metadata_superblock *disk_super;
	struct dm_block *sblock;
	__le32 csum_le;

	r = dm_bm_read_lock(bm, METADATA_SUPERBLOCK_LOCATION,
			    NULL, &sblock);
	if (r)
		goto out;

	disk_super = dm_block_data(sblock);

	csum_le = cpu_to_le32(dm_bm_checksum(&disk_super->flags,
					     sizeof(struct metadata_superblock)
					     - sizeof(__le32),
					     SUPERBLOCK_CSUM_XOR));

	if (csum_le != disk_super->csum) {
		DMERR("Superblock checksum verification failed");
		goto bad_sb;
	}

	if (le64_to_cpu(disk_super->magic) != DM_DEDUP_MAGIC) {
		DMERR("Magic number mismatch");
		goto bad_sb;
	}

	if (disk_super->version != DM_DEDUP_VERSION) {
		DMERR("Version number mismatch");
		/*
		 * XXX: handle version upgrade in future if possible
		 */
		goto bad_sb;
	}

	if (le32_to_cpu(disk_super->data_block_size) != METADATA_BSIZE) {
		DMERR("Data block size mismatch");
		goto bad_sb;
	}

	if (le32_to_cpu(disk_super->metadata_block_size) != METADATA_BSIZE) {
		DMERR("Metadata block size mismatch");
		goto bad_sb;
	}

	/* if clean shutdown flag is not set return error */
	if (!(disk_super->flags & (1 << CLEAN_SHUTDOWN)))
		DMWARN("Possible data Inconsistency. Run dmdedup_corruption_check tool");

	goto unlock_superblock;

bad_sb:
	r = -1;

unlock_superblock:
	dm_bm_unlock(sblock);

out:
	return r;
}

static struct metadata *init_meta_cowbtree(void *input_param, bool *unformatted)
{
	int ret;
	struct metadata *md;
	struct dm_block_manager *meta_bm;
	struct dm_space_map *meta_sm;
	struct dm_space_map *data_sm = NULL;
	struct dm_transaction_manager *tm;
	struct init_param_cowbtree *p =
				(struct init_param_cowbtree *)input_param;

	DMINFO("Initializing COWBTREE backend");

	md = kzalloc(sizeof(*md), GFP_NOIO);
	if (!md)
		return ERR_PTR(-ENOMEM);

	meta_bm = dm_block_manager_create(p->metadata_bdev, METADATA_BSIZE,
					  METADATA_MAXLOCKS);
	if (IS_ERR(meta_bm)) {
		md = (struct metadata *)meta_bm;
		goto badbm;
	}

	ret = superblock_all_zeroes(meta_bm, unformatted);
	if (ret) {
		md = ERR_PTR(ret);
		goto badtm;
	}

	if (!*unformatted) {
		struct dm_block *sblock;
		struct metadata_superblock *disk_super;

		DMINFO("Reconstruct DDUP device");

		ret = verify_superblock(meta_bm);
		if (ret < 0) {
			DMERR("superblock verification failed");
			/* XXX: handle error */
		}

		md->meta_bm = meta_bm;

		ret = dm_bm_read_lock(meta_bm, METADATA_SUPERBLOCK_LOCATION,
				      NULL, &sblock);
		if (ret < 0) {
			DMERR("could not read_lock superblock");
			/* XXX: handle error */
		}

		disk_super = dm_block_data(sblock);

		ret = dm_tm_open_with_sm(meta_bm, METADATA_SUPERBLOCK_LOCATION,
					 disk_super->metadata_space_map_root,
					 sizeof(
					 disk_super->metadata_space_map_root),
					 &md->tm, &md->meta_sm);
		if (ret < 0) {
			DMERR("could not open_with_sm superblock");
			/* XXX: handle error */
		}

		md->data_sm = dm_sm_disk_open(md->tm,
					      disk_super->data_space_map_root,
					      sizeof(
					      disk_super->data_space_map_root));
		if (IS_ERR(md->data_sm)) {
			DMERR("dm_disk_open failed");
			/*XXX: handle error */
		}

		dm_bm_unlock(sblock);

		goto begin_trans;
	}

	ret = dm_tm_create_with_sm(meta_bm, METADATA_SUPERBLOCK_LOCATION,
				   &tm, &meta_sm);
	if (ret < 0) {
		md = ERR_PTR(ret);
		goto badtm;
	}

	data_sm = dm_sm_disk_create(tm, p->blocks);
	if (IS_ERR(data_sm)) {
		md = (struct metadata *)data_sm;
		goto badsm;
	}

	md->meta_bm = meta_bm;
	md->tm = tm;
	md->meta_sm = meta_sm;
	md->data_sm = data_sm;

	ret = write_initial_superblock(md);
	if (ret < 0) {
		md = ERR_PTR(ret);
		goto badwritesuper;
	}

begin_trans:
	ret = __begin_transaction(md);
	if (ret < 0) {
		md = ERR_PTR(ret);
		goto badwritesuper;
	}

	md->kvs_linear = NULL;
	md->kvs_sparse = NULL;

	return md;

badwritesuper:
	dm_sm_destroy(data_sm);
badsm:
	dm_tm_destroy(tm);
	dm_sm_destroy(meta_sm);
badtm:
	dm_block_manager_destroy(meta_bm);
badbm:
	kfree(md);
	return md;
}

static void exit_meta_cowbtree(struct metadata *md)
{
	int ret;

	bool clean_shutdown_flag = true;
	ret = __commit_transaction(md, clean_shutdown_flag);
	if (ret < 0)
		DMWARN("%s: __commit_transaction() failed, error = %d.",
		       __func__, ret);

	dm_sm_destroy(md->data_sm);
	dm_tm_destroy(md->tm);
	dm_sm_destroy(md->meta_sm);
	dm_block_manager_destroy(md->meta_bm);

	kfree(md->kvs_linear);
	kfree(md->kvs_sparse);

	kfree(md);
}

static int flush_meta_cowbtree(struct metadata *md)
{
	int r;

	bool clean_shutdown_flag = false;
	r = __commit_transaction(md, clean_shutdown_flag);
	if (r < 0)
		return r;

	r = __begin_transaction(md);

	return r;
}

/********************************************************
 *		Space Management Functions		*
 ********************************************************/

static int alloc_data_block_cowbtree(struct metadata *md, uint64_t *blockn)
{
	return dm_sm_new_block(md->data_sm, blockn);
}

static int inc_refcount_cowbtree(struct metadata *md, uint64_t blockn)
{
	return dm_sm_inc_block(md->data_sm, blockn);
}

static int dec_refcount_cowbtree(struct metadata *md, uint64_t blockn)
{
	return dm_sm_dec_block(md->data_sm, blockn);
}

static int get_refcount_cowbtree(struct metadata *md, uint64_t blockn)
{
	u32 refcount;
	int r;

	r = dm_sm_get_count(md->data_sm, blockn, &refcount);
	if (r < 0)
		return r;

	return (int)refcount;
}

/*
 * This function checks if an entry is marked as deleted
 * (tombstone) or not.  We check if every byte in the
 * entry holds the value of DELETED_ENTRY.
 */
bool is_deleted_entry(const char *ptr, uint32_t length)
{
	int i = 0;

	while ((i < length) && (ptr[i] == DELETED_ENTRY))
		i++;

	return i == length;
}

/*********************************************************
 *		Linear KVS Functions			 *
 *********************************************************/

static int kvs_delete_linear_cowbtree(struct kvstore *kvs,
				      void *key, int32_t ksize)
{
	int r;
	struct kvstore_cbt_linear *kvcbt = NULL;

	kvcbt = container_of(kvs, struct kvstore_cbt_linear, ckvs);

	if (ksize != kvs->ksize)
		return -EINVAL;

	r = dm_btree_remove(&(kvcbt->info), kvcbt->root, key, &(kvcbt->root));

	if (r == -ENODATA)
		return -ENODEV;
	else if (r >= 0)
		return 0;

	return r;
}

/*
 * 0 - not found
 * 1 - found
 * < 0 - error on lookup
 */
static int kvs_lookup_linear_cowbtree(struct kvstore *kvs, void *key,
				      s32 ksize, void *value, int32_t *vsize)
{
	int r;
	struct kvstore_cbt_linear *kvcbt = NULL;

	kvcbt = container_of(kvs, struct kvstore_cbt_linear, ckvs);

	if (ksize != kvs->ksize)
		return -EINVAL;

	r = dm_btree_lookup(&(kvcbt->info), kvcbt->root, key, value);

	if (r == -ENODATA)
		return 0;
	else if (r >= 0)
		return 1;
	else
		return r;
}

static int kvs_insert_linear_cowbtree(struct kvstore *kvs, void *key,
				      s32 ksize, void *value,
				      int32_t vsize)
{
	int inserted;
	struct kvstore_cbt_linear *kvcbt = NULL;

	kvcbt = container_of(kvs, struct kvstore_cbt_linear, ckvs);

	if (ksize != kvs->ksize)
		return -EINVAL;

	if (vsize != kvs->vsize)
		return -EINVAL;

	__dm_bless_for_disk(value);
	return dm_btree_insert_notify(&(kvcbt->info), kvcbt->root, key,
				      value, &(kvcbt->root), &inserted);
}

static struct kvstore *kvs_create_linear_cowbtree(struct metadata *md,
						  u32 ksize, uint32_t vsize,
						  u32 kmax,
						  bool unformatted)
{
	struct kvstore_cbt_linear *kvs;
	int r;

	if (!vsize || !ksize)
		return ERR_PTR(-ENOTSUPP);

	/* Currently only 64bit keys are supported */
	if (ksize != 8)
		return ERR_PTR(-ENOTSUPP);

	/* We do not support two or more KVSs at the moment */
	if (md->kvs_linear)
		return ERR_PTR(-EBUSY);

	kvs = kmalloc(sizeof(*kvs), GFP_NOIO);
	if (!kvs)
		return ERR_PTR(-ENOMEM);

	kvs->ckvs.ksize = ksize;
	kvs->ckvs.vsize = vsize;

	kvs->info.tm = md->tm;
	kvs->info.levels = 1;
	kvs->info.value_type.context = NULL;
	kvs->info.value_type.size = vsize;
	kvs->info.value_type.inc = NULL;
	kvs->info.value_type.dec = NULL;
	kvs->info.value_type.equal = NULL;

	if (!unformatted) {
		kvs->ckvs.kvs_insert = kvs_insert_linear_cowbtree;
		kvs->ckvs.kvs_lookup = kvs_lookup_linear_cowbtree;
		kvs->ckvs.kvs_delete = kvs_delete_linear_cowbtree;
		kvs->ckvs.kvs_iterate = NULL;

		md->kvs_linear = kvs;
		__begin_transaction(md);
	} else {
		r = dm_btree_empty(&(kvs->info), &(kvs->root));
		if (r < 0) {
			kvs = ERR_PTR(r);
			goto badtree;
		}

		/* I think this should be moved below the 4 lines below */
		flush_meta_cowbtree(md);

		kvs->ckvs.kvs_insert = kvs_insert_linear_cowbtree;
		kvs->ckvs.kvs_lookup = kvs_lookup_linear_cowbtree;
		kvs->ckvs.kvs_delete = kvs_delete_linear_cowbtree;
		kvs->ckvs.kvs_iterate = NULL;

		md->kvs_linear = kvs;
	}

	return &(kvs->ckvs);

badtree:
	kfree(kvs);
	return (struct kvstore *)kvs;
}

/********************************************************
 *		Sparse KVS Functions			*
 ********************************************************/

/*
 * It deletes the exact entry whose keyval is provided as
 * an input. No lookup is done here.
 *
 * Returns -ERR code in failure.
 * Returns 0 on success.
 */
static int kvs_delete_entry(struct kvstore_cbt_sparse *kvcbt,
			    char *cur_entry, char *next_entry,
			    u64 cur_key_val, int ret_next)
{
	int r;

	if (ret_next == 0 &&
	    memcmp(cur_entry, next_entry, sizeof(cur_key_val)) == 0) {
		/* There is a next key and it is a linearly probed one. */
		memset(cur_entry, DELETED_ENTRY, kvcbt->entry_size);
			       __dm_bless_for_disk(&cur_key_val);

		r = dm_btree_insert(&(kvcbt->info), kvcbt->root,
				    &cur_key_val, cur_entry, &(kvcbt->root));
		DMWARN("Marked as tombstone for keyval = %lld", cur_key_val);
	} else {
		/*
		 * There is a next key and it is not a linearly probed one.
		 * OR
		 * There is no next key.
		 */
		r = dm_btree_remove(&(kvcbt->info),
				    kvcbt->root,
				    &cur_key_val,
				    &(kvcbt->root));
		DMWARN("Performed actual deletion for keyval = %lld",
		       cur_key_val);
	}
	return r;
}

static int kvs_delete_sparse_cowbtree(struct kvstore *kvs,
				      void *key, int32_t ksize)
{
	char *cur_entry, *next_entry;
	u64 key_val, cur_key_val;
	int r = 0;
	struct kvstore_cbt_sparse *kvcbt = NULL;

	kvcbt = container_of(kvs, struct kvstore_cbt_sparse, ckvs);

	if (ksize != kvs->ksize)
		return -EINVAL;

	cur_entry = kmalloc(kvcbt->entry_size, GFP_NOIO);
	if (!cur_entry)
		return -ENOMEM;

	key_val = (*(uint64_t *)key);

	r = dm_btree_lookup(&(kvcbt->info), kvcbt->root, &key_val, cur_entry);

	if (r == -ENODATA) {
		return -ENODEV;
	}
	while (r == 0) {
		cur_key_val = key_val;
		key_val++;

		next_entry = kmalloc(kvcbt->entry_size, GFP_NOIO);
		if (!next_entry)
			return -ENOMEM;

		r = dm_btree_lookup(&(kvcbt->info),
				     kvcbt->root,
				     &key_val, next_entry);

		if (!memcmp(cur_entry, key, ksize)) {
			/* Key found. */
			r = kvs_delete_entry(kvcbt, cur_entry, next_entry,
					     cur_key_val, r);
			DMWARN("Deleted key successfully\n");
			goto out;
		} else if (r == 0) {
			/* Key not found but there is a next key. */
			cur_entry = next_entry;
		} else {
			break;
		}
	}
out:
	kfree(cur_entry);
	kfree(next_entry);
	return r;
}

/*
 * 0 - not found
 * 1 - found
 * < 0 - error on lookup
 */
static int kvs_lookup_sparse_cowbtree(struct kvstore *kvs, void *key,
				      s32 ksize, void *value, int32_t *vsize)
{
	char *entry;
	u64 key_val;
	int i, r;
	struct kvstore_cbt_sparse *kvcbt = NULL;

	kvcbt = container_of(kvs, struct kvstore_cbt_sparse, ckvs);

	if (ksize != kvs->ksize)
		return -EINVAL;

	entry = kmalloc(kvcbt->entry_size, GFP_NOIO);
	if (!entry)
		return -ENOMEM;

	key_val = (*(uint64_t *)key);
	/*
	 * In case of linear probing we need to iterate only till current set
	 * lpmax. Need to put lock around whole code since multiple threads
	 * might be accessing this limit.
	 */
	for (i = 0; i <= kvcbt->curr_lpmax; i++) {
		r = dm_btree_lookup(&(kvcbt->info), kvcbt->root, &key_val,
		entry);
		if (r == -ENODATA) {
			kfree(entry);
			return 0;
		} else if (r >= 0) {
			if (!memcmp(entry, key, ksize)) {
				memcpy(value, entry + ksize, kvs->vsize);
				kfree(entry);
				return 1;
			}
			DMWARN("kvs_lookup_sparse_cowbtree: hash collision for "
			"key :%llu %s", key_val, entry);
			key_val++;
		} else {
			kfree(entry);
			return r;
		}
	}
	kfree(entry);
	return 0;
}

static int kvs_insert_sparse_cowbtree(struct kvstore *kvs, void *key,
				      s32 ksize, void *value,
				      int32_t vsize)
{
	char *entry;
	u64 key_val;
	int i, r;
	struct kvstore_cbt_sparse *kvcbt = NULL;

	kvcbt = container_of(kvs, struct kvstore_cbt_sparse, ckvs);

	if (ksize != kvs->ksize)
		return -EINVAL;

	if (vsize != kvs->vsize)
		return -EINVAL;

	entry = kmalloc(kvcbt->entry_size, GFP_NOIO);
	if (!entry)
		return -ENOMEM;

	key_val = (*(uint64_t *)key);

	for (i = 0; i <= kvcbt->lpmax; i++) {
		r = dm_btree_lookup(&(kvcbt->info), kvcbt->root, &key_val,
		entry);
		if (r == -ENODATA ||
			is_deleted_entry(entry, kvcbt->entry_size)) {
			memcpy(entry, key, ksize);
			memcpy(entry + ksize, value, vsize);
			__dm_bless_for_disk(&key_val);
			r = dm_btree_insert(&(kvcbt->info), kvcbt->root,
			&key_val, entry, &(kvcbt->root));
			kfree(entry);
			if (i > kvcbt->curr_lpmax) {
				/*
				 * TODO: Need to put locks around it since
				 * multiple threads might  read/write this
				 * variable.
				 */
				DMINFO("Changing linear probing to %d", i);
				kvcbt->curr_lpmax = i;
			}
			return r;
		} else if (r >= 0) {
			DMINFO("Collision detected for key: %s",(char *)key);
			key_val++;
		} else {
			kfree(entry);
			return r;
		}
	}
	DMWARN("Linear probing hard limit hit for insert.");
	DMINFO("Changing linear probing to hard limit :%d", kvcbt->lpmax);
	kvcbt->curr_lpmax = kvcbt->lpmax; //TODO:Check if locks required
	kfree(entry);
	return -ENOSPC;

}

static int kvs_iterate_sparse_cowbtree(struct kvstore *kvs,
				       int (*fn)(void *key, int32_t ksize,
						 void *value, s32 vsize,
						 void *data),
					void *dc)
{
	struct kvstore_cbt_sparse *kvcbt = NULL;
	char *entry, *key, *value;
	int r;
	dm_block_t lowest, highest;

	/*
	 * For unit testing, we use dconfig to iterate over the
	 * hash-pbn entries, and count is used to mark the second
	 * and forth probed entry as a deleted entry (tombstone).
	 */
#ifdef __INJECT_ERROR__
	struct dedup_config *dconfig;
	int count = 0;
	bool first = true;
	char *next_entry;
	u64 key_val;
#endif

	kvcbt = container_of(kvs, struct kvstore_cbt_sparse, ckvs);

	entry = kmalloc(kvs->ksize + kvs->vsize, GFP_NOIO);
	if (!entry)
		goto out;

	key = kmalloc(kvs->ksize, GFP_NOIO);
	if (!key)
		goto out;

	value = kmalloc(kvs->vsize, GFP_NOIO);
	if (!value)
		goto out;

	/* Get the lowest and highest keys in the key value store */
	r = dm_btree_find_lowest_key(&(kvcbt->info), kvcbt->root, &lowest);
	if (r <= 0)
		goto out;

	r = dm_btree_find_highest_key(&(kvcbt->info), kvcbt->root, &highest);
	if (r <= 0)
		goto out;

	while (lowest <= highest) {
		/* Get the next entry entry in the kvs store */
		r = dm_btree_lookup_next(&(kvcbt->info), kvcbt->root,
					 &lowest, &lowest, (void *)entry);

		lowest++;
		/*
		 * Do not iterate over entries that are marked as deleted
		 */
		if (r || is_deleted_entry(entry, kvs->ksize + kvs->vsize))
			continue;

		/* Split the key and value separately */
		memcpy(key, entry, kvs->ksize);
		memcpy(value, (void *)(entry + kvs->ksize), kvs->vsize);
/*
 * For unit testing, we mark the middle (second and forth) probed
 * entries as deleted.
 */
#ifdef __INJECT_ERROR__
		if (memcmp(key, "xxxxxxxx", 8) == 0) {
			DMWARN("iterate key: %s", key);

			if (first) {
				key_val = (*(uint64_t *)key);
				first = false;
			}
			count++;
			key_val++;

			if (count == 5) {
				/*
				 * Unit testing for the following scenario:
				 * cur_entry = last entry in probe chain,
				 * next_entry = entry not part of the chain.
				 */
				next_entry = kmalloc(kvcbt->entry_size,
						     GFP_NOIO);
				memset(next_entry, 1, kvcbt->entry_size);
				dm_btree_insert(&(kvcbt->info), kvcbt->root,
						&key_val, next_entry,
						&(kvcbt->root));
			}
			if (count % 2 == 0 || count == 5) {
				dconfig = (struct dedup_config *)dc;
				dconfig->kvs_hash_pbn->kvs_delete(
					dconfig->kvs_hash_pbn, (void *)key,
					kvs->ksize);
				DMINFO("Deleted key: %s", key);
			}
		}
#else
		/* Call the cleanup callback function */
		r = fn((void *)key, kvs->ksize, (void *)value,
		       kvs->vsize, (void *)dc);
		if (r)
			goto out;
#endif
	}

out:
	kfree(value);
	kfree(key);
	kfree(entry);

	return r;
}

static struct kvstore *kvs_create_sparse_cowbtree(struct metadata *md,
						  u32 ksize, uint32_t vsize,
						  u32 knummax,
						  bool unformatted)
{
	struct kvstore_cbt_sparse *kvs;
	int r;

	if (!vsize || !ksize)
		return ERR_PTR(-ENOTSUPP);

	/* We do not support two or more KVSs at the moment */
	if (md->kvs_sparse)
		return ERR_PTR(-EBUSY);

	kvs = kmalloc(sizeof(*kvs), GFP_NOIO);
	if (!kvs)
		return ERR_PTR(-ENOMEM);

	kvs->ckvs.vsize = vsize;
	kvs->ckvs.ksize = ksize;
	kvs->entry_size = vsize + ksize;

	kvs->info.tm = md->tm;
	kvs->info.levels = 1;
	kvs->info.value_type.context = NULL;
	kvs->info.value_type.size = kvs->entry_size;
	kvs->info.value_type.inc = NULL;
	kvs->info.value_type.dec = NULL;
	kvs->info.value_type.equal = NULL;
	kvs->lpmax = MAX_LINEAR_PROBING_LIMIT;
	kvs->curr_lpmax = MAX_LINEAR_PROBING_INITIALIZER;

	if (!unformatted) {
		kvs->ckvs.kvs_insert = kvs_insert_sparse_cowbtree;
		kvs->ckvs.kvs_lookup = kvs_lookup_sparse_cowbtree;
		kvs->ckvs.kvs_delete = kvs_delete_sparse_cowbtree;
		kvs->ckvs.kvs_iterate = kvs_iterate_sparse_cowbtree;

		md->kvs_sparse = kvs;
		__begin_transaction(md);
	} else {
		r = dm_btree_empty(&(kvs->info), &(kvs->root));
		if (r < 0) {
			kvs = ERR_PTR(r);
			goto badtree;
		}

		/* I think this should be moved below the 4 lines below */
		flush_meta_cowbtree(md);

		kvs->ckvs.kvs_insert = kvs_insert_sparse_cowbtree;
		kvs->ckvs.kvs_lookup = kvs_lookup_sparse_cowbtree;
		kvs->ckvs.kvs_delete = kvs_delete_sparse_cowbtree;
		kvs->ckvs.kvs_iterate = kvs_iterate_sparse_cowbtree;

		md->kvs_sparse = kvs;
	}

	return &(kvs->ckvs);

badtree:
	kfree(kvs);
	return (struct kvstore *)kvs;
}

int get_private_data_cowbtree(struct metadata *md, void **data, uint32_t size)
{
	if (size > sizeof(md->private_data))
		return -1;

	memcpy(*data, md->private_data, size);
	return 0;
}

int set_private_data_cowbtree(struct metadata *md, void *data, uint32_t size)
{
	if (size > sizeof(md->private_data))
		return -1;

	memcpy(md->private_data, data, size);
	return 0;
}

struct metadata_ops metadata_ops_cowbtree = {
	.init_meta = init_meta_cowbtree,
	.exit_meta = exit_meta_cowbtree,
	.kvs_create_linear = kvs_create_linear_cowbtree,
	.kvs_create_sparse = kvs_create_sparse_cowbtree,

	.alloc_data_block = alloc_data_block_cowbtree,
	.inc_refcount = inc_refcount_cowbtree,
	.dec_refcount = dec_refcount_cowbtree,
	.get_refcount = get_refcount_cowbtree,

	.flush_meta = flush_meta_cowbtree,

	.flush_bufio_cache = NULL,
	.get_private_data = get_private_data_cowbtree,
	.set_private_data = set_private_data_cowbtree,

};
