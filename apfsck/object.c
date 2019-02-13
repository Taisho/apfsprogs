/*
 *  apfsprogs/apfsck/object.c
 *
 * Author: Gabriel Krisman Bertazi <krisman@collabora.co.uk>
 *
 * Checksum routines for an APFS object
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "apfsck.h"
#include "btree.h"
#include "object.h"
#include "super.h"

static u64 fletcher64(void *addr, unsigned long len)
{
	__le32 *buff = addr;
	u64 sum1 = 0;
	u64 sum2 = 0;
	u64 c1, c2;
	int i;

	for (i = 0; i < len/sizeof(u32); i++) {
		sum1 += le32_to_cpu(buff[i]);
		sum2 += sum1;
	}

	c1 = sum1 + sum2;
	c1 = 0xFFFFFFFF - c1 % 0xFFFFFFFF;
	c2 = sum1 + c1;
	c2 = 0xFFFFFFFF - c2 % 0xFFFFFFFF;

	return (c2 << 32) | c1;
}

int obj_verify_csum(struct apfs_obj_phys *obj)
{
	return  (le64_to_cpu(obj->o_cksum) ==
		 fletcher64((char *) obj + APFS_MAX_CKSUM_SIZE,
			    sb->s_blocksize - APFS_MAX_CKSUM_SIZE));
}

/**
 * read_object - Read an object header from disk
 * @oid:	object id
 * @omap:	root of the object map (NULL if no translation is needed)
 * @obj:	object struct to receive the results
 *
 * Returns a pointer to the raw data of the object in memory.
 */
void *read_object(u64 oid, struct node *omap, struct object *obj)
{
	struct apfs_obj_phys *raw;
	struct omap_record omap_rec;
	u64 bno;
	u64 xid;
	u32 storage_type;

	if (omap) {
		omap_lookup(omap, oid, &omap_rec);
		bno = omap_rec.bno;
	} else {
		bno = oid;
	}

	raw = mmap(NULL, sb->s_blocksize, PROT_READ, MAP_PRIVATE,
		   fd, bno * sb->s_blocksize);
	if (raw == MAP_FAILED) {
		perror(NULL);
		exit(1);
	}

	if (oid != le64_to_cpu(raw->o_oid))
		report("Object header", "wrong object id in block 0x%llx.",
		       (unsigned long long)bno);
	if (oid < APFS_OID_RESERVED_COUNT)
		report("Object header", "reserved object id in block 0x%llx.",
		       (unsigned long long)bno);

	xid = le64_to_cpu(raw->o_xid);
	if (!xid || sb->s_xid < xid)
		report("Object header", "bad transaction id in block 0x%llx.",
		       (unsigned long long)bno);
	if (omap && xid != omap_rec.xid)
		report("Object header",
		       "transaction id in omap key doesn't match block 0x%llx.",
		       (unsigned long long)bno);

	obj->oid = oid;
	obj->block_nr = bno;
	obj->type = le32_to_cpu(raw->o_type) & APFS_OBJECT_TYPE_MASK;
	obj->flags = le32_to_cpu(raw->o_type) & APFS_OBJECT_TYPE_FLAGS_MASK;
	obj->subtype = le32_to_cpu(raw->o_subtype);

	/* TODO: OBJ_ENCRYPTED, OBJ_NOHEADER */
	if ((obj->flags & APFS_OBJECT_TYPE_FLAGS_DEFINED_MASK) != obj->flags)
		report("Object header", "undefined flag in use.");
	if (obj->flags & APFS_OBJ_NONPERSISTENT)
		report("Object header", "nonpersistent flag is set.");

	/* TODO: ephemeral objects? */
	storage_type = obj->flags & APFS_OBJ_STORAGETYPE_MASK;
	if (omap && storage_type != APFS_OBJ_VIRTUAL)
		report("Object header", "wrong flag for virtual object.");
	if (!omap && storage_type != APFS_OBJ_PHYSICAL)
		report("Object header", "wrong flag for physical object.");

	if (!obj_verify_csum(raw)) {
		report("Object header", "bad checksum in block 0x%llx.",
		       (unsigned long long)bno);
	}
	return raw;
}
