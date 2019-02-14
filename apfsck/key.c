/*
 *  apfsprogs/apfsck/key.c
 *
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apfsck.h"
#include "crc32c.h"
#include "types.h"
#include "key.h"
#include "super.h"
#include "unicode.h"

/**
 * read_omap_key - Parse an on-disk object map key
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	key structure to store the result
 */
void read_omap_key(void *raw, int size, struct key *key)
{
	u64 xid;

	if (size != sizeof(struct apfs_omap_key))
		report("Object map", "wrong size of key.");

	xid = le64_to_cpu(((struct apfs_omap_key *)raw)->ok_xid);
	if (!xid)
		report("Object map", "transaction id for key is zero.");

	key->id = le64_to_cpu(((struct apfs_omap_key *)raw)->ok_oid);
	key->type = 0;
	key->number = xid;
	key->name = NULL;
}

/**
 * cat_type - Read the record type of a catalog key
 * @key: the raw catalog key
 *
 * The record type is stored in the last byte of the cnid field; this function
 * returns that value.
 */
static inline int cat_type(struct apfs_key_header *key)
{
	return (le64_to_cpu(key->obj_id_and_type) & APFS_OBJ_TYPE_MASK)
			>> APFS_OBJ_TYPE_SHIFT;
}

/**
 * cat_cnid - Read the cnid value on a catalog key
 * @key: the raw catalog key
 *
 * The cnid value shares the its field with the record type. This function
 * masks that part away and returns the result.
 */
static inline u64 cat_cnid(struct apfs_key_header *key)
{
	return le64_to_cpu(key->obj_id_and_type) & APFS_OBJ_ID_MASK;
}

/**
 * keycmp - Compare two keys
 * @k1, @k2:	keys to compare
 *
 * returns   0 if @k1 and @k2 are equal
 *	   < 0 if @k1 comes before @k2 in the btree
 *	   > 0 if @k1 comes after @k2 in the btree
 */
int keycmp(struct key *k1, struct key *k2)
{
	if (k1->id != k2->id)
		return k1->id < k2->id ? -1 : 1;
	if (k1->type != k2->type)
		return k1->type < k2->type ? -1 : 1;
	if (k1->number != k2->number)
		return k1->number < k2->number ? -1 : 1;
	if (!k1->name) /* Keys of this type have no name */
		return 0;

	/* Normalization seems to be ignored here, even for directory records */
	return strcmp(k1->name, k2->name);
}

/**
 * dentry_hash - Find the key hash for a given filename
 * @name: filename to hash
 */
static u32 dentry_hash(const char *name)
{
	struct unicursor cursor;
	bool case_fold = apfs_is_case_insensitive();
	u32 hash = 0xFFFFFFFF;
	int namelen;

	init_unicursor(&cursor, name);

	while (1) {
		unicode_t utf32;

		utf32 = normalize_next(&cursor, case_fold);
		if (!utf32)
			break;

		hash = crc32c(hash, &utf32, sizeof(utf32));
	}

	/* APFS counts the NULL termination for the filename length */
	namelen = cursor.utf8curr - name;

	return ((hash & 0x3FFFFF) << 10) | (namelen & 0x3FF);
}

/**
 * read_dir_rec_key - Parse an on-disk dentry key and check its consistency
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	key structure to store the result
 */
static void read_dir_rec_key(void *raw, int size, struct key *key)
{
	struct apfs_drec_hashed_key *raw_key;
	int namelen;

	if (size < sizeof(struct apfs_drec_hashed_key) + 1)
		report("Directory record", "wrong size of key.");
	if (*((char *)raw + size - 1) != 0)
		report("Directory record", "filename lacks NULL-termination.");
	raw_key = raw;

	key->number = le32_to_cpu(raw_key->name_len_and_hash);
	key->name = (char *)raw_key->name;

	if (key->number != dentry_hash(key->name))
		report("Directory record", "filename hash is corrupted.");

	namelen = key->number & 0x3FF;
	if (strlen(key->name) + 1 != namelen) {
		/* APFS counts the NULL termination for the filename length */
		report("Directory record", "wrong name length in key.");
	}
	if (size != sizeof(struct apfs_drec_hashed_key) + namelen) {
		report("Directory record",
		       "size of key doesn't match the name length.");
	}
}

/**
 * read_xattr_key - Parse an on-disk xattr key and check its consistency
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	key structure to store the result
 */
static void read_xattr_key(void *raw, int size, struct key *key)
{
	struct apfs_xattr_key *raw_key;
	int namelen;

	if (size < sizeof(struct apfs_xattr_key) + 1)
		report("Xattr record", "wrong size of key.");
	if (*((char *)raw + size - 1) != 0)
		report("Xattr record", "name lacks NULL-termination.");
	raw_key = raw;

	key->number = 0;
	key->name = (char *)raw_key->name;

	namelen = le16_to_cpu(raw_key->name_len);
	if (strlen(key->name) + 1 != namelen) {
		/* APFS counts the NULL termination in the string length */
		report("Xattr record", "wrong name length.");
	}
	if (size != sizeof(struct apfs_xattr_key) + namelen) {
		report("Xattr record",
		       "size of key doesn't match the name length.");
	}
}

/**
 * read_snap_name_key - Parse an on-disk snapshot name key and check its
 *			consistency
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	key structure to store the result
 *
 * TODO: this is the same as read_xattr_key(), maybe they could be merged.
 */
static void read_snap_name_key(void *raw, int size, struct key *key)
{
	struct apfs_snap_name_key *raw_key;
	int namelen;

	if (size < sizeof(struct apfs_snap_name_key) + 1)
		report("Snapshot name record", "wrong size of key.");
	if (*((char *)raw + size - 1) != 0)
		report("Snapshot name record", "name lacks NULL-termination.");
	raw_key = raw;

	key->number = 0;
	key->name = (char *)raw_key->name;

	namelen = le16_to_cpu(raw_key->name_len);
	if (strlen(key->name) + 1 != namelen) {
		/* APFS counts the NULL termination in the string length */
		report("Snapshot name record", "wrong name length.");
	}
	if (size != sizeof(struct apfs_snap_name_key) + namelen) {
		report("Snapshot name record",
		       "size of key doesn't match the name length.");
	}
}

/**
 * read_file_extent_key - Parse an on-disk extent key and check its consistency
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	key structure to store the result
 */
static void read_file_extent_key(void *raw, int size, struct key *key)
{
	struct apfs_file_extent_key *raw_key;

	if (size != sizeof(struct apfs_file_extent_key))
		report("Extent record", "wrong size of key.");
	raw_key = raw;

	key->number = le64_to_cpu(raw_key->logical_addr);
	key->name = NULL;
}

/**
 * read_sibling_link_key - Parse an on-disk sibling link key and check its
 *			   consistency
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	key structure to store the result
 */
static void read_sibling_link_key(void *raw, int size, struct key *key)
{
	struct apfs_sibling_link_key *raw_key;

	if (size != sizeof(struct apfs_sibling_link_key))
		report("Siblink link record", "wrong size of key.");
	raw_key = raw;

	key->number = le64_to_cpu(raw_key->sibling_id); /* Only guessing */
	key->name = NULL;
}

/**
 * read_cat_key - Parse an on-disk catalog key
 * @raw:	pointer to the raw key
 * @size:	size of the raw key
 * @key:	key structure to store the result
 */
void read_cat_key(void *raw, int size, struct key *key)
{
	if (size < sizeof(struct apfs_key_header))
		report("Catalog tree", "key is too small.");
	key->id = cat_cnid((struct apfs_key_header *)raw);
	key->type = cat_type((struct apfs_key_header *)raw);

	switch (key->type) {
	case APFS_TYPE_DIR_REC:
		read_dir_rec_key(raw, size, key);
		return;
	case APFS_TYPE_XATTR:
		read_xattr_key(raw, size, key);
		return;
	case APFS_TYPE_FILE_EXTENT:
		read_file_extent_key(raw, size, key);
		return;
	case APFS_TYPE_SNAP_NAME:
		read_snap_name_key(raw, size, key);
		return;
	case APFS_TYPE_SIBLING_LINK:
		read_sibling_link_key(raw, size, key);
		return;
	default:
		/* All other key types are just the header */
		if (size != sizeof(struct apfs_key_header))
			report("Catalog tree record", "wrong size of key.");
		key->number = 0;
		key->name = NULL;
		return;
	}
}
