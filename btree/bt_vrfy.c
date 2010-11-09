/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	uint32_t frags;			/* Total frags */
	bitstr_t *fragbits;			/* Frag tracking bit list */

	FILE	*stream;			/* Dump file stream */

	void (*f)(const char *, uint64_t);	/* Progress callback */
	uint64_t fcnt;				/* Progress counter */

	WT_PAGE *leaf;				/* Child page */
} WT_VSTUFF;

static int __wt_bt_verify_delfmt(DB *, uint32_t, uint32_t);
static int __wt_bt_verify_eof(DB *, uint32_t, uint32_t);
static int __wt_bt_verify_eop(DB *, uint32_t, uint32_t);
static int __wt_bt_verify_addfrag(DB *, WT_PAGE *, WT_VSTUFF *);
static int __wt_bt_verify_checkfrag(DB *, WT_VSTUFF *);
static int __wt_bt_verify_cmp(WT_TOC *, WT_ROW *, WT_PAGE *, int);
static int __wt_bt_verify_page_col_fix(DB *, WT_PAGE *);
static int __wt_bt_verify_page_col_int(DB *, WT_PAGE *);
static int __wt_bt_verify_page_col_rcc(DB *, WT_PAGE *);
static int __wt_bt_verify_page_desc(DB *, WT_PAGE *);
static int __wt_bt_verify_page_item(WT_TOC *, WT_PAGE *, WT_VSTUFF *);
static int __wt_bt_verify_page_ovfl(WT_TOC *, WT_PAGE *);
static int __wt_bt_verify_tree(WT_TOC *,
    WT_ROW *, uint64_t, uint32_t, WT_OFF *, WT_VSTUFF *);

/*
 * __wt_db_verify --
 *	Verify a Btree.
 */
int
__wt_db_verify(WT_TOC *toc, void (*f)(const char *, uint64_t))
{
	return (__wt_bt_verify(toc, f, NULL));
}

/*
 * __wt_bt_verify --
 *	Verify a Btree, optionally dumping each page in debugging mode.
 */
int
__wt_bt_verify(
    WT_TOC *toc, void (*f)(const char *, uint64_t), FILE *stream)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_OFF off;
	WT_PAGE *page;
	WT_VSTUFF vstuff;
	int ret;

	env = toc->env;
	db = toc->db;
	idb = db->idb;
	page = NULL;
	ret = 0;

	memset(&vstuff, 0, sizeof(vstuff));
	vstuff.stream = stream;
	vstuff.f = f;

	/*
	 * Allocate a bit array, where each bit represents a single allocation
	 * size piece of the file.   This is how we track the parts of the file
	 * we've verified.  Storing this on the heap seems reasonable: with a
	 * minimum allocation size of 512B, we would allocate 4MB to verify a
	 * 16GB file.  To verify larger files than we can handle this way, we'd
	 * have to write parts of the bit array into a disk file.
	 *
	 * !!!
	 * There's one portability issue -- the bitstring package uses "ints",
	 * not unsigned ints, or any fixed size.   If an "int" can't hold a
	 * big enough value, we could lose.   There's a check here to make we
	 * don't overflow.   I don't ever expect to see this error message, but
	 * better safe than sorry.
	 */
	vstuff.frags = WT_OFF_TO_ADDR(db, idb->fh->file_size);
	if (vstuff.frags > INT_MAX) {
		__wt_api_db_errx(db, "file is too large to verify");
		goto err;
	}
	WT_ERR(bit_alloc(env, vstuff.frags, &vstuff.fragbits));

	/*
	 * Verify the descriptor page; the descriptor page can't move, so simply
	 * retry any WT_RESTART returns.
	 *
	 * We have to keep our hazard reference on the descriptor page while we
	 * walk the tree.  The problem we're solving is if the root page
	 * were to be re-written between the time we read the descriptor page
	 * and when we read the root page, we'd read an out-of-date root page.
	 * (Other methods don't have to worry about this because they only work
	 * when the database is opened and the root page is pinned into memory.
	 * Db.verify works on both opened and unopened databases, so it has to
	 * ensure the root page doesn't move.   This is a wildly unlikely race,
	 * of course, but it's easy to handle.)
	 */
	WT_ERR_RESTART(__wt_bt_page_in(toc, 0, 512, 0, &page));
	WT_ERR(__wt_bt_verify_page(toc, page, &vstuff));

	/* Verify the tree, starting at the root from the descriptor page. */
	WT_RECORDS(&off) = 0;
	off.addr = idb->root_addr;
	off.size = idb->root_size;
	WT_ERR(__wt_bt_verify_tree(
	    toc, NULL, (uint64_t)0, WT_NOLEVEL, &off, &vstuff));

	WT_ERR(__wt_bt_verify_checkfrag(db, &vstuff));

err:	if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);
	if (vstuff.leaf != NULL)
		__wt_bt_page_out(toc, &vstuff.leaf, 0);

	/* Wrap up reporting. */
	if (vstuff.f != NULL)
		vstuff.f(toc->name, vstuff.fcnt);
	if (vstuff.fragbits != NULL)
		__wt_free(env, vstuff.fragbits, 0);

	return (ret);
}

/*
 * Callers pass us a WT_OFF structure, and a reference to the internal node
 * key that referenced that page (if any -- the root node doesn't have one).
 *
 * The plan is simple.  We recursively descend the tree, in depth-first fashion.
 * First we verify each page, so we know it is correctly formed, and any keys
 * it contains are correctly ordered.  After page verification, we check the
 * connections within the tree.
 *
 * There are two connection checks: First, we compare the internal node key that
 * lead to the current page against the first entry on the current page.  The
 * internal node key must compare less than or equal to the first entry on the
 * current page.  Second, we compare the largest key we've seen on any leaf page
 * against the next internal node key we find.  This check is a little tricky:
 * Every time we find a leaf page, we save it in the vs->leaf structure.  The
 * next time we are about to indirect through an entry on an internal node, we
 * compare the last entry on that saved page against the internal node entry's
 * key.  In that comparison, the leaf page's key must be less than the internal
 * node entry's key.
 *
 * Off-page duplicate trees are handled the same way (this function is called
 * from the page verification routine when an off-page duplicate tree is found).
 *
 * __wt_bt_verify_tree --
 *	Verify a subtree of the tree, recursively descending through the tree.
 */
static int
__wt_bt_verify_tree(WT_TOC *toc, WT_ROW *parent_rip,
    uint64_t start_recno, uint32_t level, WT_OFF *off, WT_VSTUFF *vs)
{
	DB *db;
	WT_COL *cip;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_ROW *rip;
	int is_root, ret;

	db = toc->db;
	page = NULL;
	ret = 0;

	/*
	 * If passed a level of WT_NOLEVEL, that is, the only level that can't
	 * possibly be a valid database page level, this is the root page of
	 * the tree.
	 */
	is_root = level == WT_NOLEVEL ? 1 : 0;

	/*
	 * Read and verify the page.
	 *
	 * If the page were to be rewritten/discarded from the cache while
	 * we're getting it, we can re-try -- re-trying is safe because our
	 * addr/size information is from a page which can't be discarded
	 * because of our hazard reference.  If the page was re-written, our
	 * on-page overflow information will have been updated to the overflow
	 * page's new address.
	 */
	WT_RET_RESTART(__wt_bt_page_in(toc, off->addr, off->size, 0, &page));
	WT_ERR(__wt_bt_verify_page(toc, page, vs));

	/*
	 * The page is OK, instantiate its in-memory information if we don't
	 * already have it.
	 */
	if (page->u.indx == NULL)
		WT_ERR(__wt_bt_page_inmem(db, page));

	hdr = page->hdr;

	/*
	 * If it's the root, use this page's level to initialize expected the
	 * values for the rest of the tree; otherwise, check that tree levels
	 * and record counts match up.
	 */
	if (is_root)
		level = hdr->level;
	else {
		if (hdr->level != level) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a tree level of %lu where "
			    "the expected level was %lu",
			    (u_long)off->addr,
			    (u_long)hdr->level, (u_long)level);
			goto err;
		}

		/*
		 * This check isn't strictly an on-disk format check, but it's
		 * useful to confirm that the number of records found on this
		 * page (by summing the WT_OFF structure record counts) matches
		 * the WT_OFF structure record count in our parent.  We could
		 * sum as we walk the page below, but we did that when bringing
		 * the page into memory, there's no reason to do it again.
		 */
		if (page->records != WT_RECORDS(off)) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a record count of %llu where "
			    "the expected record count was %llu",
			    (u_long)off->addr, page->records,
			    (unsigned long long)WT_RECORDS(off));
			goto err;
		}
	}

	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
		/*
		 * In column stores we need to confirm the starting record
		 * number on the child page is correct.
		 */
		if (is_root)
			start_recno = 1;
		if (hdr->start_recno != start_recno) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a starting record of %llu "
			    "where the expected starting record was %llu",
			    (u_long)off->addr,
			    (unsigned long long)hdr->start_recno,
			    (unsigned long long)start_recno);
			goto err;
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		/* Row stores never have non-zero starting record numbers. */
		if (hdr->start_recno != 0) {
			__wt_api_db_errx(db,
			    "page at addr %lu has a starting record of %llu, "
			    "which should never be non-zero",
			    (u_long)off->addr,
			    (unsigned long long)hdr->start_recno);
			goto err;
		}
		/*
		 * In row stores we're passed the parent page's key referencing
		 * this page: it must sort less than or equal to the first key
		 * on this page.
		 */
		if (!is_root)
			WT_ERR(__wt_bt_verify_cmp(toc, parent_rip, page, 1));
		break;
	default:
		break;
	}

	/*
	 * Leaf pages need no further processing; in the case of row-store leaf
	 * pages, we'll need them to check their last entry against the next
	 * internal key in the tree; save a reference and return.
	 */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
		__wt_bt_page_out(toc, &page, 0);
		return (0);
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		vs->leaf = page;
		return (0);
	default:
		break;
	}

	/* For each entry in the internal page, verify the subtree. */
	switch (hdr->type) {
	uint32_t i;
	case WT_PAGE_COL_INT:
		start_recno = hdr->start_recno;
		WT_INDX_FOREACH(page, cip, i) {
			WT_ERR(__wt_bt_verify_tree(
			    toc, NULL, start_recno, level - 1, cip->data, vs));
			start_recno += WT_COL_OFF_RECORDS(cip);
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, rip, i) {
			/*
			 * At each off-page entry, we compare the current entry
			 * against the largest key in the subtree rooted to the
			 * immediate left of the current item; this key must
			 * compare less than or equal to the current item.  The
			 * trick here is we need the last leaf key, not the last
			 * internal node key.  It's returned to us in the leaf
			 * field of the vs structure, whenever we verify a leaf
			 * page.  Discard the leaf node as soon as we've used it
			 * in a comparison.
			 */
			if (vs->leaf != NULL) {
				WT_ERR(
				    __wt_bt_verify_cmp(toc, rip, vs->leaf, 0));
				__wt_bt_page_out(toc, &vs->leaf, 0);
				vs->leaf = NULL;
			}
			WT_ERR(__wt_bt_verify_tree(toc, rip, (uint64_t)0,
			    level - 1, WT_ITEM_BYTE_OFF(rip->data), vs));
		}
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	/*
	 * The largest key on the last leaf page in the tree is never needed,
	 * there aren't any internal pages after it.  So, we get here with
	 * vs->leaf needing to be released.
	 */
err:	if (vs->leaf != NULL)
		__wt_bt_page_out(toc, &vs->leaf, 0);
	if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);

	return (ret);
}

/*
 * __wt_bt_verify_cmp --
 *	Compare a key on a parent page to a designated entry on a child page.
 */
static int
__wt_bt_verify_cmp(
    WT_TOC *toc, WT_ROW *parent_rip, WT_PAGE *child, int first_entry)
{
	DB *db;
	DBT *cd_ref, *pd_ref, *scratch1, *scratch2, tmp1, tmp2;
	WT_PAGE *child_ovfl_page, *parent_ovfl_page;
	WT_ROW *child_rip;
	int cmp, ret, (*func)(DB *, const DBT *, const DBT *);

	db = toc->db;
	scratch1 = scratch2 = NULL;
	child_ovfl_page = parent_ovfl_page = NULL;
	ret = 0;

	/* Set the comparison function. */
	switch (child->hdr->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		func = db->btree_compare_dup;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		func = db->btree_compare;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * The two keys we're going to compare may be overflow keys -- don't
	 * bother instantiating the keys in the tree, there's no reason to
	 * believe we're going to be working in this database.
	 */
	child_rip = first_entry ?
	    child->u.irow : child->u.irow + (child->indx_count - 1);
	if (WT_KEY_PROCESS(child_rip)) {
		WT_ERR(__wt_scr_alloc(toc, &scratch1));
		WT_ERR(__wt_bt_item_process(
		    toc, child_rip->key, &child_ovfl_page, scratch1));
		if (child_ovfl_page != NULL) {
			WT_CLEAR(tmp1);
			tmp1.data = WT_PAGE_BYTE(child_ovfl_page);
			tmp1.size = child_ovfl_page->hdr->u.datalen;
			cd_ref = &tmp1;
		} else
			cd_ref = scratch1;
	} else
		cd_ref = (DBT *)child_rip;
	if (WT_KEY_PROCESS(parent_rip)) {
		WT_ERR(__wt_scr_alloc(toc, &scratch2));
		WT_RET(__wt_bt_item_process(
		    toc, parent_rip->key, &parent_ovfl_page, scratch2));
		if (parent_ovfl_page != NULL) {
			WT_CLEAR(tmp2);
			tmp2.data = WT_PAGE_BYTE(parent_ovfl_page);
			tmp2.size = parent_ovfl_page->hdr->u.datalen;
			pd_ref = &tmp2;
		} else
			pd_ref = scratch2;
	} else
		pd_ref = (DBT *)parent_rip;

	/* Compare the parent's key against the child's key. */
	cmp = func(db, cd_ref, pd_ref);

	if (first_entry && cmp < 0) {
		__wt_api_db_errx(db,
		    "the first key on page at addr %lu sorts before its "
		    "reference key on its parent's page",
		    (u_long)child->addr);
		ret = WT_ERROR;
	}
	if (!first_entry && cmp >= 0) {
		__wt_api_db_errx(db,
		    "the last key on the page at addr %lu sorts after a parent "
		    "page's key for the subsequent page",
		    (u_long)child->addr);
		ret = WT_ERROR;
	}

err:	if (scratch1 != NULL)
		__wt_scr_release(&scratch1);
	if (scratch2 != NULL)
		__wt_scr_release(&scratch2);
	if (child_ovfl_page != NULL)
		__wt_bt_page_out(toc, &child_ovfl_page, 0);
	if (parent_ovfl_page != NULL)
		__wt_bt_page_out(toc, &parent_ovfl_page, 0);

	return (ret);
}

/*
 * __wt_bt_verify_page --
 *	Verify a single Btree page.
 */
int
__wt_bt_verify_page(WT_TOC *toc, WT_PAGE *page, void *vs_arg)
{
	DB *db;
	WT_PAGE_HDR *hdr;
	WT_VSTUFF *vs;
	uint32_t addr;

	vs = vs_arg;
	db = toc->db;

	hdr = page->hdr;
	addr = page->addr;

	/* Report progress every 10 pages. */
	if (vs != NULL && vs->f != NULL && ++vs->fcnt % 10 == 0)
		vs->f(toc->name, vs->fcnt);

	/* Update frags list. */
	if (vs != NULL && vs->fragbits != NULL)
		WT_RET(__wt_bt_verify_addfrag(db, page, vs));

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */
	if (hdr->lsn[0] != 0 || hdr->lsn[1] != 0) {
		__wt_api_db_errx(db,
		    "page at addr %lu has non-zero lsn header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/*
	 * Don't verify the checksum -- it verified when we first read the
	 * page.
	 */

	/* Check the page type. */
	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	case WT_PAGE_INVALID:
	default:
		__wt_api_db_errx(db,
		    "page at addr %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)hdr->type);
		return (WT_ERROR);
	}

	/* Check the page level. */
	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
		if (hdr->level != WT_NOLEVEL)
			goto err_level;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_LEAF:
		if (hdr->level != WT_LLEAF)
			goto err_level;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		if (hdr->level <= WT_LLEAF) {
err_level:		__wt_api_db_errx(db,
			    "%s page at addr %lu has incorrect tree level "
			    "of %lu",
			    __wt_bt_hdr_type(hdr),
			    (u_long)addr, (u_long)hdr->level);
			return (WT_ERROR);
		}
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	if (hdr->unused[0] != '\0' || hdr->unused[1] != '\0') {
		__wt_api_db_errx(db,
		    "page at addr %lu has non-zero unused header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/* Verify the items on the page. */
	switch (hdr->type) {
	case WT_PAGE_DESCRIPT:
		WT_RET(__wt_bt_verify_page_desc(db, page));
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_bt_verify_page_item(toc, page, vs));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_bt_verify_page_col_int(db, page));
		break;
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_bt_verify_page_col_fix(db, page));
		break;
	case WT_PAGE_COL_RCC:
		WT_RET(__wt_bt_verify_page_col_rcc(db, page));
		break;
	case WT_PAGE_OVFL:
		WT_RET(__wt_bt_verify_page_ovfl(toc, page));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs != NULL && vs->stream != NULL)
		return (__wt_bt_debug_page(toc, page, NULL, vs->stream));
#endif
	return (0);
}

/*
 * __wt_bt_verify_page_item --
 *	Walk a page of WT_ITEMs, and verify them.
 */
static int
__wt_bt_verify_page_item(WT_TOC *toc, WT_PAGE *page, WT_VSTUFF *vs)
{
	struct {
		uint32_t indx;			/* Item number */

		DBT	*item;			/* Item to compare */
		DBT	 item_std;		/* On-page reference */
		DBT	 item_ovfl;		/* Overflow holder */
		WT_PAGE	*ovfl;			/* Overflow page */
		DBT	 *item_comp;		/* Uncompressed holder */
	} *current, *last_data, *last_key, *swap_tmp, _a, _b, _c;
	DB *db;
	IDB *idb;
	WT_ITEM *item;
	WT_OVFL *ovfl;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	uint8_t *end;
	uint32_t addr, i, item_num, item_len, item_type;
	int (*func)(DB *, const DBT *, const DBT *), ret;

	db = toc->db;
	idb = db->idb;
	ret = 0;

	hdr = page->hdr;
	end = (uint8_t *)hdr + page->size;
	addr = page->addr;

	/*
	 * We have a maximum of 3 key/data items we track -- the last key, the
	 * last data item, and the current item.   They're stored in the _a,
	 * _b, and _c structures (it doesn't matter which) -- what matters is
	 * which item is referenced by current, last_data or last_key.
	 *
	 * If we're doing Huffman compression, allocate scratch buffers for the
	 * decompressed versions.
	 */
	WT_CLEAR(_a);
	WT_CLEAR(_b);
	WT_CLEAR(_c);
	current = &_a;
	if (idb->huffman_key != NULL || idb->huffman_data != NULL)
		WT_ERR(__wt_scr_alloc(toc, &_a.item_comp));
	last_data = &_b;
	if (idb->huffman_key != NULL || idb->huffman_data != NULL)
		WT_ERR(__wt_scr_alloc(toc, &_b.item_comp));
	last_key = &_c;
	if (idb->huffman_key != NULL || idb->huffman_data != NULL)
		WT_ERR(__wt_scr_alloc(toc, &_c.item_comp));

	/* Set the comparison function. */
	switch (hdr->type) {
	case WT_PAGE_COL_VAR:
		func = NULL;
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		func = db->btree_compare_dup;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		func = db->btree_compare;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	item_num = 0;
	WT_ITEM_FOREACH(page, item, i) {
		++item_num;

		/* Check if this item is entirely on the page. */
		if ((uint8_t *)item + sizeof(WT_ITEM) > end)
			goto eop;

		item_type = WT_ITEM_TYPE(item);
		item_len = WT_ITEM_LEN(item);

		/* Check the item's type. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (hdr->type != WT_PAGE_ROW_INT &&
			    hdr->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_KEY_DUP_OVFL:
			if (hdr->type != WT_PAGE_DUP_INT)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			if (hdr->type != WT_PAGE_COL_VAR &&
			    hdr->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA_DUP:
		case WT_ITEM_DATA_DUP_OVFL:
			if (hdr->type != WT_PAGE_DUP_LEAF &&
			    hdr->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DEL:
			/*
			 * XXX
			 * You can delete items from fixed-length pages, why
			 * aren't we checking against WT_PAGE_COL_FIX and
			 * WT_PAGE_COL_RCC here?
			 */
			if (hdr->type != WT_PAGE_COL_VAR)
				goto item_vs_page;
			break;
		case WT_ITEM_OFF:
			if (hdr->type != WT_PAGE_DUP_INT &&
			    hdr->type != WT_PAGE_ROW_INT &&
			    hdr->type != WT_PAGE_ROW_LEAF) {
item_vs_page:			__wt_api_db_errx(db,
				    "illegal item and page type combination "
				    "(item %lu on page at addr %lu is a %s "
				    "item on a %s page)",
				    (u_long)item_num, (u_long)addr,
				    __wt_bt_item_type(item),
				    __wt_bt_hdr_type(hdr));
				goto err_set;
			}
			break;
		default:
			__wt_api_db_errx(db,
			    "item %lu on page at addr %lu has an illegal type "
			    "of %lu",
			    (u_long)item_num, (u_long)addr, (u_long)item_type);
			goto err_set;
		}

		/* Check the item's length. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_DUP:
			/* The length is variable, we can't check it. */
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			if (item_len != sizeof(WT_OVFL))
				goto item_len;
			break;
		case WT_ITEM_DEL:
			if (item_len != 0)
				goto item_len;
			break;
		case WT_ITEM_OFF:
			if (item_len != sizeof(WT_OFF)) {
item_len:			__wt_api_db_errx(db,
				    "item %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)item_num, (u_long)addr);
				goto err_set;
			}
			break;
		default:
			break;
		}

		/* Check if the item is entirely on the page. */
		if ((uint8_t *)WT_ITEM_NEXT(item) > end) {
eop:			ret = __wt_bt_verify_eop(db, item_num, addr);
			goto err;
		}

		/*
		 * We set ovfl to any overflow reference, and then use it
		 * below, knowing this code initialized it.  Guarantee it
		 * is set, it makes lint happier.
		 */
		ovfl = NULL;
		switch (item_type) {
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			ovfl = WT_ITEM_BYTE_OVFL(item);
			if (WT_ADDR_TO_OFF(db, ovfl->addr) +
			    WT_HDR_BYTES_TO_ALLOC(db, ovfl->size) >
			    idb->fh->file_size)
				goto eof;
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			if (WT_ADDR_TO_OFF(db, off->addr) +
			    off->size > idb->fh->file_size) {
eof:				ret = __wt_bt_verify_eof(db, item_num, addr);
				goto err;
			}
			break;
		default:
			break;
		}

		/* Verify overflow references. */
		switch (item_type) {
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			/*
			 * Discard any previous overflow page -- if we're here,
			 * reading in a new overflow page, we must be done with
			 * the previous one.
			 */
			if (current->ovfl != NULL) {
				__wt_bt_page_out(toc, &current->ovfl, 0);
				current->ovfl = NULL;
			}

			WT_ERR(__wt_bt_ovfl_in(toc, ovfl, &current->ovfl));
			WT_ERR(__wt_bt_verify_page(toc, current->ovfl, vs));

			/*
			 * Check that the underlying overflow page's size
			 * is correct.
			 */
			if (ovfl->size != current->ovfl->hdr->u.datalen) {
				__wt_api_db_errx(db,
				    "overflow page reference in item %lu on "
				    "page at addr %lu does not match the data "
				    "size on the overflow page",
				    (u_long)item_num, (u_long)addr);
				goto err_set;
			}
			break;
		default:
			break;
		}

		/*
		 * Check the page item sort order.  If the page doesn't contain
		 * sorted items (or, if the item is an off-page item and we're
		 * not verifying the entire tree), continue walking the page
		 * items.   Otherwise, get a DBT that represents the item and
		 * compare it with the last item.
		 */
		switch (item_type) {
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DEL:
		case WT_ITEM_OFF:
			/*
			 * These items aren't sorted on the page-- we're done.
			 */
			goto offpagedups;
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_DATA_DUP:
			current->indx = item_num;
			current->item = &current->item_std;
			current->item->data = WT_ITEM_BYTE(item);
			current->item->size = item_len;
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_KEY_DUP_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			/*
			 * We already have a copy of the overflow page, read in
			 * when the overflow page was verified.  Set our DBT
			 * to reference it.
			 */
			current->indx = item_num;
			current->item = &current->item_ovfl;
			current->item_ovfl.data = WT_PAGE_BYTE(current->ovfl);
			current->item_ovfl.size = ovfl->size;
			break;
		default:
			break;
		}

		/* Get a decompressed version if necessary. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			/* If key is compressed, get an uncompressed copy. */
			if (idb->huffman_key != NULL) {
				WT_ERR(__wt_huffman_decode(idb->huffman_key,
				    current->item->data, current->item->size,
				    &current->item_comp->data,
				    &current->item_comp->mem_size,
				    &current->item_comp->size));
				current->item = current->item_comp;
			}
			break;
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_DATA_DUP:
		case WT_ITEM_DATA_DUP_OVFL:
			/* If data is compressed, get an uncompressed copy. */
			if (idb->huffman_data != NULL) {
				WT_ERR(__wt_huffman_decode(idb->huffman_data,
				    current->item->data, current->item->size,
				    &current->item_comp->data,
				    &current->item_comp->mem_size,
				    &current->item_comp->size));
				current->item = current->item_comp;
			}
			break;
		default:	/* No other values are possible. */
			break;
		}

		/* Check the sort order. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_DUP:
		case WT_ITEM_KEY_OVFL:
			if (last_key->item != NULL &&
			    func(db, last_key->item, current->item) >= 0) {
				__wt_api_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted",
				    last_key->indx, current->indx,
				    (u_long)addr);
				goto err_set;
			}
			swap_tmp = last_key;
			last_key = current;
			current = swap_tmp;
			break;
		case WT_ITEM_DATA_DUP:
		case WT_ITEM_DATA_DUP_OVFL:
			if (last_data->item != NULL &&
			    func(db, last_data->item, current->item) >= 0) {
				__wt_api_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted",
				    last_data->indx, current->indx,
				    (u_long)addr);
				goto err_set;
			}
			swap_tmp = last_data;
			last_data = current;
			current = swap_tmp;
			break;
		default:	/* No other values are possible. */
			break;
		}

offpagedups:	/*
		 * If we're verifying the entire tree, verify any off-page
		 * duplicate trees (that's any off-page references found on
		 * a row-store leaf page).
		 */
		if (vs != NULL && hdr->type == WT_PAGE_ROW_LEAF)
			switch (item_type) {
			case WT_ITEM_OFF:
				off = WT_ITEM_BYTE_OFF(item);
				WT_ERR(__wt_bt_verify_tree(toc,
				    NULL, (uint64_t)0, WT_NOLEVEL, off, vs));
				break;
			default:
				break;
			}
	}

	if (0) {
err_set:	ret = WT_ERROR;
	}

err:	/* Discard any overflow pages we're still holding. */
	if (_a.ovfl != NULL)
		__wt_bt_page_out(toc, &_a.ovfl, 0);
	if (_b.ovfl != NULL)
		__wt_bt_page_out(toc, &_b.ovfl, 0);
	if (_c.ovfl != NULL)
		__wt_bt_page_out(toc, &_c.ovfl, 0);

	/* Discard any scratch buffers we allocated. */
	if (_a.item_comp != NULL)
		__wt_scr_release(&_a.item_comp);
	if (_b.item_comp != NULL)
		__wt_scr_release(&_b.item_comp);
	if (_c.item_comp != NULL)
		__wt_scr_release(&_c.item_comp);

	return (ret);
}

/*
 * __wt_bt_verify_page_col_int --
 *	Walk a WT_PAGE_COL_INT page and verify it.
 */
static int
__wt_bt_verify_page_col_int(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_OFF *off;
	WT_PAGE_HDR *hdr;
	uint8_t *end;
	uint32_t addr, i, entry_num;

	idb = db->idb;
	hdr = page->hdr;
	end = (uint8_t *)hdr + page->size;
	addr = page->addr;

	entry_num = 0;
	WT_OFF_FOREACH(page, off, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if ((uint8_t *)off + sizeof(WT_OFF) > end)
			return (__wt_bt_verify_eop(db, entry_num, addr));

		/* Check if the reference is past the end-of-file. */
		if (WT_ADDR_TO_OFF(
		    db, off->addr) + off->size > idb->fh->file_size)
			return (__wt_bt_verify_eof(db, entry_num, addr));
	}

	return (0);
}

/*
 * __wt_bt_verify_page_col_fix --
 *	Walk a WT_PAGE_COL_FIX page and verify it.
 */
static int
__wt_bt_verify_page_col_fix(DB *db, WT_PAGE *page)
{
	u_int len;
	uint32_t addr, i, j, entry_num;
	uint8_t *data, *end, *p;

	len = db->fixed_len;
	end = (uint8_t *)page->hdr + page->size;
	addr = page->addr;

	entry_num = 0;
	WT_FIX_FOREACH(db, page, data, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if (data + len > end)
			return (__wt_bt_verify_eop(db, entry_num, addr));

		/* Deleted items are entirely nul bytes. */
		p = data;
		if (WT_FIX_DELETE_ISSET(data)) {
			if (*p != WT_FIX_DELETE_BYTE)
				goto delfmt;
			for (j = 1; j < db->fixed_len; ++j)
				if (*++p != '\0')
					goto delfmt;
		}
	}

	return (0);

delfmt:	return (__wt_bt_verify_delfmt(db, entry_num, addr));
}

/*
 * __wt_bt_verify_page_col_rcc --
 *	Walk a WT_PAGE_COL_RCC page and verify it.
 */
static int
__wt_bt_verify_page_col_rcc(DB *db, WT_PAGE *page)
{
	u_int len;
	uint32_t addr, i, j, entry_num;
	uint8_t *data, *end, *last_data, *p;

	end = (uint8_t *)page->hdr + page->size;
	addr = page->addr;

	last_data = NULL;
	len = db->fixed_len + sizeof(uint16_t);

	entry_num = 0;
	WT_RCC_REPEAT_FOREACH(db, page, data, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if (data + len > end)
			return (__wt_bt_verify_eop(db, entry_num, addr));

		/* Count must be non-zero. */
		if (WT_RCC_REPEAT_COUNT(data) == 0) {
			__wt_api_db_errx(db,
			    "fixed-length entry %lu on page at addr "
			    "%lu has a repeat count of 0",
			    (u_long)entry_num, (u_long)addr);
			return (WT_ERROR);
		}

		/* Deleted items are entirely nul bytes. */
		p = WT_RCC_REPEAT_DATA(data);
		if (WT_FIX_DELETE_ISSET(p)) {
			if (*p != WT_FIX_DELETE_BYTE)
				goto delfmt;
			for (j = 1; j < db->fixed_len; ++j)
				if (*++p != '\0')
					goto delfmt;
		}

		/*
		 * If the previous data is the same as this data, we
		 * missed an opportunity for compression -- complain.
		 */
		if (last_data != NULL &&
		    memcmp(WT_RCC_REPEAT_DATA(last_data),
		    WT_RCC_REPEAT_DATA(data), db->fixed_len) == 0 &&
		    WT_RCC_REPEAT_COUNT(last_data) < UINT16_MAX) {
			__wt_api_db_errx(db,
			    "fixed-length entries %lu and %lu on page "
			    "at addr %lu are identical and should have "
			    "been compressed",
			    (u_long)entry_num,
			    (u_long)entry_num - 1, (u_long)addr);
			return (WT_ERROR);
		}
		last_data = data;
	}

	return (0);

delfmt:	return (__wt_bt_verify_delfmt(db, entry_num, addr));
}

/*
 * __wt_bt_verify_page_desc --
 *	Verify the database description on page 0.
 */
static int
__wt_bt_verify_page_desc(DB *db, WT_PAGE *page)
{
	WT_PAGE_DESC *desc;
	u_int i;
	uint8_t *p;
	int ret;

	ret = 0;

	desc = (WT_PAGE_DESC *)WT_PAGE_BYTE(page);
	if (desc->magic != WT_BTREE_MAGIC) {
		__wt_api_db_errx(db, "magic number %#lx, expected %#lx",
		    (u_long)desc->magic, WT_BTREE_MAGIC);
		ret = WT_ERROR;
	}
	if (desc->majorv != WT_BTREE_MAJOR_VERSION) {
		__wt_api_db_errx(db, "major version %d, expected %d",
		    (int)desc->majorv, WT_BTREE_MAJOR_VERSION);
		ret = WT_ERROR;
	}
	if (desc->minorv != WT_BTREE_MINOR_VERSION) {
		__wt_api_db_errx(db, "minor version %d, expected %d",
		    (int)desc->minorv, WT_BTREE_MINOR_VERSION);
		ret = WT_ERROR;
	}
	if (desc->intlmin != db->intlmin) {
		__wt_api_db_errx(db,
		    "minimum internal page size %lu, expected %lu",
		    (u_long)db->intlmin, (u_long)desc->intlmin);
		ret = WT_ERROR;
	}
	if (desc->intlmax != db->intlmax) {
		__wt_api_db_errx(db,
		    "maximum internal page size %lu, expected %lu",
		    (u_long)db->intlmax, (u_long)desc->intlmax);
		ret = WT_ERROR;
	}
	if (desc->leafmin != db->leafmin) {
		__wt_api_db_errx(db, "minimum leaf page size %lu, expected %lu",
		    (u_long)db->leafmin, (u_long)desc->leafmin);
		ret = WT_ERROR;
	}
	if (desc->leafmax != db->leafmax) {
		__wt_api_db_errx(db, "maximum leaf page size %lu, expected %lu",
		    (u_long)db->leafmax, (u_long)desc->leafmax);
		ret = WT_ERROR;
	}
	if (desc->recno_offset != 0) {
		__wt_api_db_errx(db, "recno offset %llu, expected 0",
		    (unsigned long long)desc->recno_offset);
		ret = WT_ERROR;
	}
	if (F_ISSET(desc, ~WT_PAGE_DESC_MASK)) {
		__wt_api_db_errx(db,
		    "unexpected flags found in description record");
		ret = WT_ERROR;
	}
	if (desc->fixed_len == 0 && F_ISSET(desc, WT_PAGE_DESC_REPEAT)) {
		__wt_api_db_errx(db,
		    "repeat counts configured but no fixed length record "
		    "size specified");
		ret = WT_ERROR;
	}

	for (p = (uint8_t *)desc->unused1,
	    i = sizeof(desc->unused1); i > 0; --i)
		if (*p != '\0')
			goto unused_not_clear;
	for (p = (uint8_t *)desc->unused2,
	    i = sizeof(desc->unused2); i > 0; --i)
		if (*p != '\0') {
unused_not_clear:	__wt_api_db_errx(db,
			    "unexpected values found in description record's "
			    "unused fields");
			ret = WT_ERROR;
		}

	return (ret);
}

/*
 * __wt_bt_verify_page_ovfl --
 *	Verify a WT_PAGE_OVFL page.
 */
static int
__wt_bt_verify_page_ovfl(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	WT_PAGE_HDR *hdr;
	uint32_t addr, len;
	uint8_t *p;

	db = toc->db;
	hdr = page->hdr;
	addr = page->addr;

	if (hdr->u.datalen == 0) {
		__wt_api_db_errx(db,
		    "overflow page at addr %lu has no data", (u_long)addr);
		return (WT_ERROR);
	}

	/* Any page data after the overflow record should be nul bytes. */
	p = (uint8_t *)hdr + (sizeof(WT_PAGE_HDR) + hdr->u.datalen);
	len = page->size - (sizeof(WT_PAGE_HDR) + hdr->u.datalen);
	for (; len > 0; ++p, --len)
		if (*p != '\0') {
			__wt_api_db_errx(db,
			    "overflow page at addr %lu has non-zero trailing "
			    "bytes",
			    (u_long)addr);
			return (WT_ERROR);
		}

	return (0);
}

/*
 * __wt_bt_verify_eop --
 *	Generic item extends past the end-of-page error.
 */
static int
__wt_bt_verify_eop(DB *db, uint32_t entry_num, uint32_t addr)
{
	__wt_api_db_errx(db,
	    "item %lu on page at addr %lu extends past the end of the page",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_bt_verify_eof --
 *	Generic item references non-existent file pages error.
 */
static int
__wt_bt_verify_eof(DB *db, uint32_t entry_num, uint32_t addr)
{
	__wt_api_db_errx(db,
	    "off-page item %lu on page at addr %lu references non-existent "
	    "file pages",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_bt_verify_delfmt --
 *	WT_PAGE_COL_FIX and WT_PAGE_COL_RCC error where a deleted item has
 *	non-nul bytes.
 */
static int
__wt_bt_verify_delfmt(DB *db, uint32_t entry_num, uint32_t addr)
{
	__wt_api_db_errx(db,
	    "deleted fixed-length entry %lu on page at addr %lu has non-nul "
	    "bytes",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_bt_verify_addfrag --
 *	Add a new set of fragments to the list, and complain if we've already
 *	verified this chunk of the file.
 */
static int
__wt_bt_verify_addfrag(DB *db, WT_PAGE *page, WT_VSTUFF *vs)
{
	uint32_t addr, frags, i;

	addr = page->addr;
	frags = WT_OFF_TO_ADDR(db, page->size);
	for (i = 0; i < frags; ++i)
		if (bit_test(vs->fragbits, addr + i)) {
			__wt_api_db_errx(db,
			    "page fragment at addr %lu already verified",
			    (u_long)addr);
			return (WT_ERROR);
		}
	bit_nset(vs->fragbits, addr, addr + (frags - 1));
	return (0);
}

/*
 * __wt_bt_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_bt_verify_checkfrag(DB *db, WT_VSTUFF *vs)
{
	int ffc, ffc_start, ffc_end, frags, ret;

	frags = (int)vs->frags;		/* XXX: bitstring.h wants "ints" */
	ret = 0;

	/* Check for page fragments we haven't verified. */
	for (ffc_start = ffc_end = -1;;) {
		bit_ffc(vs->fragbits, frags, &ffc);
		if (ffc != -1) {
			bit_set(vs->fragbits, ffc);
			if (ffc_start == -1) {
				ffc_start = ffc_end = ffc;
				continue;
			}
			if (ffc_end == ffc - 1) {
				ffc_end = ffc;
				continue;
			}
		}
		if (ffc_start != -1) {
			if (ffc_start == ffc_end)
				__wt_api_db_errx(db,
				    "fragment %d was never verified",
				    ffc_start);
			else
				__wt_api_db_errx(db,
				    "fragments %d to %d were never verified",
				    ffc_start, ffc_end);
			ret = WT_ERROR;
		}
		ffc_start = ffc_end = ffc;
		if (ffc == -1)
			break;
	}
	return (ret);
}
