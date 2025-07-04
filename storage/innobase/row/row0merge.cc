/*****************************************************************************

Copyright (c) 2005, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2023, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file row/row0merge.cc
New index creation routines using a merge sort

Created 12/4/2005 Jan Lindstrom
Completed by Sunny Bains and Marko Makela
*******************************************************/
#include <my_global.h>
#include <log.h>
#include <sql_class.h>
#include <math.h>

#include "row0merge.h"
#include "row0ext.h"
#include "row0log.h"
#include "row0ins.h"
#include "row0row.h"
#include "row0sel.h"
#include "log0crypt.h"
#include "dict0crea.h"
#include "trx0purge.h"
#include "lock0lock.h"
#include "pars0pars.h"
#include "ut0sort.h"
#include "row0ftsort.h"
#include "row0import.h"
#include "row0vers.h"
#include "handler0alter.h"
#include "btr0bulk.h"
#ifdef BTR_CUR_ADAPT
# include "btr0sea.h"
#endif /* BTR_CUR_ADAPT */
#include "ut0stage.h"
#include "fil0crypt.h"
#include "srv0mon.h"

/* Ignore posix_fadvise() on those platforms where it does not exist */
#if defined _WIN32
# define posix_fadvise(fd, offset, len, advice) /* nothing */
#endif /* _WIN32 */

/* Whether to disable file system cache */
char	srv_disable_sort_file_cache;

/** Class that caches spatial index row tuples made from a single cluster
index page scan, and then insert into corresponding index tree */
class spatial_index_info {
public:
  /** constructor
  @param index	spatial index to be created */
  spatial_index_info(dict_index_t *index) : index(index)
  {
    ut_ad(index->is_spatial());
  }

  /** Caches an index row into index tuple vector
  @param[in]	row	table row
  @param[in]	ext	externally stored column prefixes, or NULL */
  void add(const dtuple_t *row, const row_ext_t *ext, mem_heap_t *heap)
  {
    dtuple_t *dtuple= row_build_index_entry(row, ext, index, heap);
    ut_ad(dtuple);
    ut_ad(dtuple->n_fields == index->n_fields);
    if (ext)
    {
      /* Replace any references to ext, because ext will be allocated
      from row_heap. */
      for (ulint i= 1; i < dtuple->n_fields; i++)
      {
        dfield_t &dfield= dtuple->fields[i];
        if (dfield.data >= ext->buf &&
            dfield.data <= &ext->buf[ext->n_ext * ext->max_len])
          dfield_dup(&dfield, heap);
      }
    }
    m_dtuple_vec.push_back(dtuple);
  }

	/** Insert spatial index rows cached in vector into spatial index
	@param[in]	trx_id		transaction id
	@param[in]	pcur		cluster index scanning cursor
	@param[in,out]	mtr_started	whether scan_mtr is active
	@param[in,out]	heap		temporary memory heap
	@param[in,out]	scan_mtr	mini-transaction for pcur
	@return DB_SUCCESS if successful, else error number */
	dberr_t insert(trx_id_t trx_id, btr_pcur_t* pcur,
		       bool& mtr_started, mem_heap_t* heap, mtr_t* scan_mtr)
	{
		big_rec_t*      big_rec;
		rec_t*          rec;
		btr_cur_t       ins_cur;
		mtr_t           mtr;
		rtr_info_t      rtr_info;
		rec_offs*	ins_offsets = NULL;
		dberr_t		error = DB_SUCCESS;
		dtuple_t*	dtuple;
		const ulint	flag = BTR_NO_UNDO_LOG_FLAG
				       | BTR_NO_LOCKING_FLAG
				       | BTR_KEEP_SYS_FLAG | BTR_CREATE_FLAG;

		ut_ad(mtr_started == scan_mtr->is_active());

		DBUG_EXECUTE_IF("row_merge_instrument_log_check_flush",
				log_sys.set_check_for_checkpoint(););

		for (idx_tuple_vec::iterator it = m_dtuple_vec.begin();
		     it != m_dtuple_vec.end();
		     ++it) {
			dtuple = *it;
			ut_ad(dtuple);

			if (log_sys.check_for_checkpoint()) {
				if (mtr_started) {
					if (!btr_pcur_move_to_prev_on_page(pcur)) {
						error = DB_CORRUPTION;
						break;
					}
					btr_pcur_store_position(pcur, scan_mtr);
					scan_mtr->commit();
					mtr_started = false;
				}

				log_free_check();
			}

			mtr.start();
			index->set_modified(mtr);

			ins_cur.page_cur.index = index;
			rtr_init_rtr_info(&rtr_info, false, &ins_cur, index,
					  false);
			rtr_info_update_btr(&ins_cur, &rtr_info);

			error = rtr_insert_leaf(&ins_cur, nullptr, dtuple,
						BTR_MODIFY_LEAF, &mtr);

			/* It need to update MBR in parent entry,
			so change search mode to BTR_MODIFY_TREE */
			if (error == DB_SUCCESS && rtr_info.mbr_adj) {
				mtr.commit();
				rtr_clean_rtr_info(&rtr_info, true);
				rtr_init_rtr_info(&rtr_info, false, &ins_cur,
						  index, false);
				rtr_info_update_btr(&ins_cur, &rtr_info);
				mtr.start();
				index->set_modified(mtr);
				error = rtr_insert_leaf(&ins_cur, nullptr,
							dtuple,
							BTR_MODIFY_TREE, &mtr);
			}

			if (error == DB_SUCCESS) {
				error = btr_cur_optimistic_insert(
					flag, &ins_cur, &ins_offsets,
					&heap, dtuple, &rec, &big_rec,
					0, NULL, &mtr);
			}

			ut_ad(!big_rec);

			if (error == DB_FAIL) {
				mtr.commit();
				mtr.start();
				index->set_modified(mtr);

				rtr_clean_rtr_info(&rtr_info, true);
				rtr_init_rtr_info(&rtr_info, false,
						  &ins_cur, index, false);

				rtr_info_update_btr(&ins_cur, &rtr_info);
				error = rtr_insert_leaf(&ins_cur, nullptr,
							dtuple,
							BTR_MODIFY_TREE, &mtr);

				if (error == DB_SUCCESS) {
					error = btr_cur_pessimistic_insert(
						flag, &ins_cur, &ins_offsets,
						&heap, dtuple, &rec,
						&big_rec, 0, NULL, &mtr);
				}
			}

			ut_ad(!big_rec);

			DBUG_EXECUTE_IF(
				"row_merge_ins_spatial_fail",
				error = DB_FAIL;
			);

			if (error == DB_SUCCESS) {
				if (rtr_info.mbr_adj) {
					error = rtr_ins_enlarge_mbr(
							&ins_cur, &mtr);
				}

				if (error == DB_SUCCESS) {
					page_update_max_trx_id(
						btr_cur_get_block(&ins_cur),
						btr_cur_get_page_zip(&ins_cur),
						trx_id, &mtr);
				}
			}

			mtr.commit();

			rtr_clean_rtr_info(&rtr_info, true);
		}

		m_dtuple_vec.clear();

		return(error);
	}

private:
  /** Cache index rows made from a cluster index scan. Usually
  for rows on single cluster index page */
  typedef std::vector<dtuple_t*, ut_allocator<dtuple_t*> > idx_tuple_vec;

  /** vector used to cache index rows made from cluster index scan */
  idx_tuple_vec m_dtuple_vec;
public:
  /** the index being built */
  dict_index_t*const	index;
};

/* Maximum pending doc memory limit in bytes for a fts tokenization thread */
#define FTS_PENDING_DOC_MEMORY_LIMIT	1000000

/** Insert sorted data tuples to the index.
@param[in]	index		index to be inserted
@param[in]	old_table	old table
@param[in]	fd		file descriptor
@param[in,out]	block		file buffer
@param[in]	row_buf		row_buf the sorted data tuples,
or NULL if fd, block will be used instead
@param[in,out]	btr_bulk	btr bulk instance
@param[in]	table_total_rows total rows of old table
@param[in]	pct_progress	total progress percent untill now
@param[in]	pct_cost	current progress percent
@param[in]	crypt_block	buffer for encryption or NULL
@param[in]	space		space id
@param[in,out]	stage		performance schema accounting object, used by
ALTER TABLE. If not NULL stage->begin_phase_insert() will be called initially
and then stage->inc() will be called for each record that is processed.
@param[in]	blob_file	To read big column field data from
				the given blob file. It is
				applicable only for bulk insert
				operation
@return DB_SUCCESS or error number */
static	MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_merge_insert_index_tuples(
	dict_index_t*		index,
	const dict_table_t*	old_table,
	const pfs_os_file_t&	fd,
	row_merge_block_t*	block,
	const row_merge_buf_t*	row_buf,
	BtrBulk*		btr_bulk,
	const ib_uint64_t	table_total_rows,
	double			pct_progress,
	double			pct_cost,
	row_merge_block_t*	crypt_block,
	ulint			space,
	ut_stage_alter_t*	stage= nullptr,
	merge_file_t*		blob_file= nullptr);

/** Encode an index record.
@return size of the record */
static MY_ATTRIBUTE((nonnull))
ulint
row_merge_buf_encode(
/*=================*/
	byte**			b,		/*!< in/out: pointer to
						current end of output buffer */
	const dict_index_t*	index,		/*!< in: index */
	const mtuple_t*		entry,		/*!< in: index fields
						of the record to encode */
	ulint			n_fields)	/*!< in: number of fields
						in the entry */
{
	ulint	size;
	ulint	extra_size;

	size = rec_get_converted_size_temp<false>(
		index, entry->fields, n_fields, &extra_size);
	ut_ad(size >= extra_size);

	/* Encode extra_size + 1 */
	if (extra_size + 1 < 0x80) {
		*(*b)++ = (byte) (extra_size + 1);
	} else {
		ut_ad((extra_size + 1) < 0x8000);
		*(*b)++ = (byte) (0x80 | ((extra_size + 1) >> 8));
		*(*b)++ = (byte) (extra_size + 1);
	}

	rec_convert_dtuple_to_temp<false>(*b + extra_size, index,
				   entry->fields, n_fields);

	*b += size;
	return size;
}

static MY_ATTRIBUTE((malloc, nonnull))
row_merge_buf_t*
row_merge_buf_create_low(
  row_merge_buf_t *buf, mem_heap_t *heap, dict_index_t *index)
{
  ulint max_tuples = srv_sort_buf_size
                     / std::max<ulint>(1, dict_index_get_min_size(index));
  ut_ad(max_tuples > 0);
  ut_ad(max_tuples <= srv_sort_buf_size);

  buf->heap = heap;
  buf->index = index;
  buf->max_tuples = max_tuples;
  buf->tuples = static_cast<mtuple_t*>(
   ut_malloc_nokey(2 * max_tuples * sizeof *buf->tuples));
  buf->tmp_tuples = buf->tuples + max_tuples;
  return(buf);
}

/******************************************************//**
Allocate a sort buffer.
@return own: sort buffer */
row_merge_buf_t*
row_merge_buf_create(
/*=================*/
	dict_index_t*	index)	/*!< in: secondary index */
{
	row_merge_buf_t*	buf;
	ulint			buf_size;
	mem_heap_t*		heap;

	buf_size = (sizeof *buf);

	heap = mem_heap_create(buf_size);

	buf = static_cast<row_merge_buf_t*>(
		mem_heap_zalloc(heap, buf_size));
	row_merge_buf_create_low(buf, heap, index);

	return(buf);
}

/******************************************************//**
Empty a sort buffer.
@return sort buffer */
row_merge_buf_t*
row_merge_buf_empty(
/*================*/
	row_merge_buf_t*	buf)	/*!< in,own: sort buffer */
{
	ulint		buf_size	= sizeof *buf;
	ulint		max_tuples	= buf->max_tuples;
	mem_heap_t*	heap		= buf->heap;
	dict_index_t*	index		= buf->index;
	mtuple_t*	tuples		= buf->tuples;

	mem_heap_empty(heap);

	buf = static_cast<row_merge_buf_t*>(mem_heap_zalloc(heap, buf_size));
	buf->heap = heap;
	buf->index = index;
	buf->max_tuples = max_tuples;
	buf->tuples = tuples;
	buf->tmp_tuples = buf->tuples + max_tuples;

	return(buf);
}

/******************************************************//**
Deallocate a sort buffer. */
void
row_merge_buf_free(
/*===============*/
	row_merge_buf_t*	buf)	/*!< in,own: sort buffer to be freed */
{
	ut_free(buf->tuples);
	mem_heap_free(buf->heap);
}

/** Convert the field data from compact to redundant format.
@param[in]	row_field	field to copy from
@param[out]	field		field to copy to
@param[in]	len		length of the field data
@param[in]	zip_size	compressed BLOB page size,
				zero for uncompressed BLOBs
@param[in,out]	heap		memory heap where to allocate data when
				converting to ROW_FORMAT=REDUNDANT, or NULL
				when not to invoke
				row_merge_buf_redundant_convert(). */
static
void
row_merge_buf_redundant_convert(
	const dfield_t*		row_field,
	dfield_t*		field,
	ulint			len,
	ulint			zip_size,
	mem_heap_t*		heap)
{
	ut_ad(field->type.mbminlen == 1);
	ut_ad(field->type.mbmaxlen > 1);

	byte*		buf = (byte*) mem_heap_alloc(heap, len);
	ulint		field_len = row_field->len;
	ut_ad(field_len <= len);

	if (row_field->ext) {
		const byte*	field_data = static_cast<const byte*>(
			dfield_get_data(row_field));
		ulint		ext_len;

		ut_a(field_len >= BTR_EXTERN_FIELD_REF_SIZE);
		ut_a(memcmp(field_data + field_len - BTR_EXTERN_FIELD_REF_SIZE,
			    field_ref_zero, BTR_EXTERN_FIELD_REF_SIZE));

		byte*	data = btr_copy_externally_stored_field(
			&ext_len, field_data, zip_size, field_len, heap);

		ut_ad(ext_len < len);

		memcpy(buf, data, ext_len);
		field_len = ext_len;
	} else {
		memcpy(buf, row_field->data, field_len);
	}

	memset(buf + field_len, 0x20, len - field_len);

	dfield_set_data(field, buf, len);
}

/** Insert the tuple into bulk buffer insert operation
@param	buf	merge buffer for the index operation
@param	table	bulk insert operation for the table
@param	row	tuple to be inserted
@return number of rows inserted */
static ulint row_merge_bulk_buf_add(row_merge_buf_t* buf,
                                    const dict_table_t &table,
                                    const dtuple_t &row)
{
  if (buf->n_tuples >= buf->max_tuples)
    return 0;

  const dict_index_t *index= buf->index;
  ulint n_fields= dict_index_get_n_fields(index);
  mtuple_t *entry= &buf->tuples[buf->n_tuples];
  ulint data_size= 0;
  ulint extra_size= UT_BITS_IN_BYTES(unsigned(index->n_nullable));
  dfield_t *field= entry->fields= static_cast<dfield_t*>(
     mem_heap_alloc(buf->heap, n_fields * sizeof *entry->fields));
  const dict_field_t *ifield= dict_index_get_nth_field(index, 0);

  for (ulint i = 0; i < n_fields; i++, field++, ifield++)
  {
    dfield_copy(field, &row.fields[i]);
    ulint len= dfield_get_len(field);
    const dict_col_t* const col= ifield->col;

    if (dfield_is_null(field))
      continue;

    ulint fixed_len= ifield->fixed_len;

    /* CHAR in ROW_FORMAT=REDUNDANT is always
    fixed-length, but in the temporary file it is
    variable-length for variable-length character sets. */
    if (fixed_len && !index->table->not_redundant() &&
        col->mbminlen != col->mbmaxlen)
      fixed_len= 0;

    if (fixed_len);
    else if (len < 128 || (!DATA_BIG_COL(col)))
      extra_size++;
    else
      extra_size += 2;
    data_size += len;
  }

  /* Add to the total size of the record in row_merge_block_t
  the encoded length of extra_size and the extra bytes (extra_size).
  See row_merge_buf_write() for the variable-length encoding
  of extra_size. */
  data_size += (extra_size + 1) + ((extra_size + 1) >= 0x80);

  /* Reserve bytes for the end marker of row_merge_block_t. */
  if (buf->total_size + data_size >= srv_sort_buf_size)
    return 0;

  buf->total_size += data_size;
  buf->n_tuples++;

  field= entry->fields;

  do
    dfield_dup(field++, buf->heap);
  while (--n_fields);

  return 1;
}

/** Insert a data tuple into a sort buffer.
@param[in,out]	buf		sort buffer
@param[in]	fts_index	fts index to be created
@param[in]	old_table	original table
@param[in]	new_table	new table
@param[in,out]	psort_info	parallel sort info
@param[in,out]	row		table row
@param[in]	ext		cache of externally stored
				column prefixes, or NULL
@param[in,out]	doc_id		Doc ID if we are creating
				FTS index
@param[in,out]	conv_heap	memory heap where to allocate data when
				converting to ROW_FORMAT=REDUNDANT, or NULL
				when not to invoke
				row_merge_buf_redundant_convert()
@param[in,out]	err		set if error occurs
@param[in,out]	v_heap		heap memory to process data for virtual column
@param[in,out]	my_table	mysql table object
@param[in]	trx		transaction object
@param[in]	col_collate	columns whose collations changed, or nullptr
@return number of rows added, 0 if out of space */
static
ulint
row_merge_buf_add(
	row_merge_buf_t*	buf,
	dict_index_t*		fts_index,
	const dict_table_t*	old_table,
	const dict_table_t*	new_table,
	fts_psort_t*		psort_info,
	dtuple_t*		row,
	const row_ext_t*	ext,
	doc_id_t*		doc_id,
	mem_heap_t*		conv_heap,
	dberr_t*		err,
	mem_heap_t**		v_heap,
	TABLE*			my_table,
	trx_t*			trx,
	const col_collations*	col_collate)
{
	ulint			i;
	const dict_index_t*	index;
	mtuple_t*		entry;
	dfield_t*		field;
	const dict_field_t*	ifield;
	ulint			n_fields;
	ulint			data_size;
	ulint			extra_size;
	ulint			bucket = 0;
	doc_id_t		write_doc_id;
	ulint			n_row_added = 0;
	VCOL_STORAGE		vcol_storage;

	DBUG_ENTER("row_merge_buf_add");

	if (buf->n_tuples >= buf->max_tuples) {
error:
		n_row_added = 0;
		goto end;
	}

	DBUG_EXECUTE_IF(
		"ib_row_merge_buf_add_two",
		if (buf->n_tuples >= 2) DBUG_RETURN(0););

	UNIV_PREFETCH_R(row->fields);

	/* If we are building FTS index, buf->index points to
	the 'fts_sort_idx', and real FTS index is stored in
	fts_index */
	index = (buf->index->type & DICT_FTS) ? fts_index : buf->index;

	/* create spatial index should not come here */
	ut_ad(!dict_index_is_spatial(index));

	n_fields = dict_index_get_n_fields(index);

	entry = &buf->tuples[buf->n_tuples];
	field = entry->fields = static_cast<dfield_t*>(
		mem_heap_alloc(buf->heap, n_fields * sizeof *entry->fields));

	data_size = 0;
	extra_size = UT_BITS_IN_BYTES(unsigned(index->n_nullable));

	ifield = dict_index_get_nth_field(index, 0);

	for (i = 0; i < n_fields; i++, field++, ifield++) {
		ulint			len;
		ulint			fixed_len;
		const dfield_t*		row_field;
		const dict_col_t* const col = ifield->col;
		const dict_v_col_t* const v_col = col->is_virtual()
			? reinterpret_cast<const dict_v_col_t*>(col)
			: NULL;

		/* Process the Doc ID column */
		if (!v_col && *doc_id
		    && col->ind == index->table->fts->doc_col) {
			fts_write_doc_id((byte*) &write_doc_id, *doc_id);

			/* Note: field->data now points to a value on the
			stack: &write_doc_id after dfield_set_data(). Because
			there is only one doc_id per row, it shouldn't matter.
			We allocate a new buffer before we leave the function
			later below. */

			dfield_set_data(
				field, &write_doc_id, sizeof(write_doc_id));

			field->type.mtype = ifield->col->mtype;
			field->type.prtype = ifield->col->prtype;
			field->type.mbminlen = 0;
			field->type.mbmaxlen = 0;
			field->type.len = ifield->col->len;
		} else {
			/* Use callback to get the virtual column value */
			if (v_col) {
				dict_index_t*	clust_index
					= dict_table_get_first_index(new_table);

                                if (!vcol_storage.innobase_record &&
                                    !innobase_allocate_row_for_vcol(
						trx->mysql_thd, clust_index,
						v_heap, &my_table,
						&vcol_storage)) {
					*err = DB_OUT_OF_MEMORY;
					goto error;
				}

				row_field = innobase_get_computed_value(
					row, v_col, clust_index,
					v_heap, NULL, ifield, trx->mysql_thd,
					my_table, vcol_storage.innobase_record,
					old_table, NULL);

				if (row_field == NULL) {
					*err = DB_COMPUTE_VALUE_FAILED;
					goto error;
				}
				dfield_copy(field, row_field);
			} else {
				row_field = dtuple_get_nth_field(row,
								 col->ind);
				dfield_copy(field, row_field);

				/* Copy the column collation to the
				tuple field */
				if (col_collate) {
					auto it = col_collate->find(col->ind);
					if (it != col_collate->end()) {
						field->type
							.assign(*it->second);
					}
				}
			}

			/* Tokenize and process data for FTS */
			if (index->type & DICT_FTS) {
				fts_doc_item_t*	doc_item;
				byte*		value;
				void*		ptr;
				const ulint	max_trial_count = 10000;
				ulint		trial_count = 0;

				/* fetch Doc ID if it already exists
				in the row, and not supplied by the
				caller. Even if the value column is
				NULL, we still need to get the Doc
				ID so to maintain the correct max
				Doc ID */
				if (*doc_id == 0) {
					const dfield_t*	doc_field;
					doc_field = dtuple_get_nth_field(
						row,
						index->table->fts->doc_col);
					*doc_id = (doc_id_t) mach_read_from_8(
						static_cast<const byte*>(
						dfield_get_data(doc_field)));

					if (*doc_id == 0) {
						ib::warn() << "FTS Doc ID is"
							" zero. Record"
							" skipped";
						goto error;
					}
				}

				if (dfield_is_null(field)) {
					n_row_added = 1;
					continue;
				}

				ptr = ut_malloc_nokey(sizeof(*doc_item)
						      + field->len);

				doc_item = static_cast<fts_doc_item_t*>(ptr);
				value = static_cast<byte*>(ptr)
					+ sizeof(*doc_item);
				memcpy(value, field->data, field->len);
				field->data = value;

				doc_item->field = field;
				doc_item->doc_id = *doc_id;

				bucket = static_cast<ulint>(
					*doc_id % fts_sort_pll_degree);

				/* Add doc item to fts_doc_list */
				mysql_mutex_lock(&psort_info[bucket].mutex);

				if (psort_info[bucket].error == DB_SUCCESS) {
					UT_LIST_ADD_LAST(
						psort_info[bucket].fts_doc_list,
						doc_item);
					psort_info[bucket].memory_used +=
						sizeof(*doc_item) + field->len;
				} else {
					ut_free(doc_item);
				}

				mysql_mutex_unlock(&psort_info[bucket].mutex);

				/* Sleep when memory used exceeds limit*/
				while (psort_info[bucket].memory_used
				       > FTS_PENDING_DOC_MEMORY_LIMIT
				       && trial_count++ < max_trial_count) {
					std::this_thread::sleep_for(
						std::chrono::milliseconds(1));
				}

				n_row_added = 1;
				continue;
			}

			/* innobase_get_computed_value() sets the
			length of the virtual column field. */
			if (v_col == NULL
			    && field->len != UNIV_SQL_NULL
			    && col->mtype == DATA_MYSQL
			    && col->len != field->len) {
				if (conv_heap != NULL) {
					row_merge_buf_redundant_convert(
						row_field, field, col->len,
						old_table->space->zip_size(),
						conv_heap);
				}
			}
		}

		len = dfield_get_len(field);

		if (dfield_is_null(field)) {
			ut_ad(!(col->prtype & DATA_NOT_NULL));
			continue;
		} else if (!ext) {
		} else if (dict_index_is_clust(index)) {
			/* Flag externally stored fields. */
			const byte*	buf = row_ext_lookup(ext, col->ind,
							     &len);
			if (UNIV_LIKELY_NULL(buf)) {
				if (UNIV_UNLIKELY(buf == field_ref_zero)) {
					*err = DB_CORRUPTION;
					goto error;
				}
				if (i < dict_index_get_n_unique(index)) {
					dfield_set_data(field, buf, len);
				} else {
					dfield_set_ext(field);
					len = dfield_get_len(field);
				}
			}
		} else if (!v_col) {
			/* Only non-virtual column are stored externally */
			const byte*	buf = row_ext_lookup(ext, col->ind,
							     &len);
			if (UNIV_LIKELY_NULL(buf)) {
				if (UNIV_UNLIKELY(buf == field_ref_zero)) {
					*err = DB_CORRUPTION;
					goto error;
				}
				dfield_set_data(field, buf, len);
			}
		}

		/* If a column prefix index, take only the prefix */

		if (ifield->prefix_len) {
			len = dtype_get_at_most_n_mbchars(
				col->prtype,
				col->mbminlen, col->mbmaxlen,
				ifield->prefix_len,
				len,
				static_cast<char*>(dfield_get_data(field)));
			dfield_set_len(field, len);
		}

		ut_ad(len <= col->len
		      || DATA_LARGE_MTYPE(col->mtype));

		fixed_len = ifield->fixed_len;
		if (fixed_len && !dict_table_is_comp(index->table)
		    && col->mbminlen != col->mbmaxlen) {
			/* CHAR in ROW_FORMAT=REDUNDANT is always
			fixed-length, but in the temporary file it is
			variable-length for variable-length character
			sets. */
			fixed_len = 0;
		}

		if (fixed_len) {
#ifdef UNIV_DEBUG
			/* len should be between size calculated based on
			mbmaxlen and mbminlen */
			ut_ad(len <= fixed_len);
			ut_ad(!col->mbmaxlen || len >= col->mbminlen
			      * (fixed_len / col->mbmaxlen));

			ut_ad(!dfield_is_ext(field));
#endif /* UNIV_DEBUG */
		} else if (dfield_is_ext(field)) {
			extra_size += 2;
		} else if (len < 128
			   || (!DATA_BIG_COL(col))) {
			extra_size++;
		} else {
			/* For variable-length columns, we look up the
			maximum length from the column itself.  If this
			is a prefix index column shorter than 256 bytes,
			this will waste one byte. */
			extra_size += 2;
		}
		data_size += len;
	}

	/* If this is FTS index, we already populated the sort buffer, return
	here */
	if (index->type & DICT_FTS) {
		goto end;
	}

#ifdef UNIV_DEBUG
	{
		ulint	size;
		ulint	extra;

		size = rec_get_converted_size_temp<false>(
			index, entry->fields, n_fields, &extra);

		ut_ad(data_size + extra_size == size);
		ut_ad(extra_size == extra);
	}
#endif /* UNIV_DEBUG */

	/* Add to the total size of the record in row_merge_block_t
	the encoded length of extra_size and the extra bytes (extra_size).
	See row_merge_buf_write() for the variable-length encoding
	of extra_size. */
	data_size += (extra_size + 1) + ((extra_size + 1) >= 0x80);

	/* Record size can exceed page size while converting to
	redundant row format. But there is assert
	ut_ad(size < srv_page_size) in rec_offs_data_size().
	It may hit the assert before attempting to insert the row. */
	if (conv_heap != NULL && data_size > srv_page_size) {
		*err = DB_TOO_BIG_RECORD;
	}

	ut_ad(data_size < srv_sort_buf_size);

	/* Reserve bytes for the end marker of row_merge_block_t. */
	if (buf->total_size + data_size >= srv_sort_buf_size) {
		goto error;
	}

	buf->total_size += data_size;
	buf->n_tuples++;
	n_row_added++;

	field = entry->fields;

	/* Copy the data fields. */

	do {
		dfield_dup(field++, buf->heap);
	} while (--n_fields);

	if (conv_heap != NULL) {
		mem_heap_empty(conv_heap);
	}

end:
        if (vcol_storage.innobase_record)
		innobase_free_row_for_vcol(&vcol_storage);
	DBUG_RETURN(n_row_added);
}

/*************************************************************//**
Report a duplicate key. */
void
row_merge_dup_report(
/*=================*/
	row_merge_dup_t*	dup,	/*!< in/out: for reporting duplicates */
	const dfield_t*		entry)	/*!< in: duplicate index entry */
{
	if (!dup->n_dup++ && dup->table) {
		/* Only report the first duplicate record,
		but count all duplicate records. */
		innobase_fields_to_mysql(dup->table, dup->index, entry);
	}
}

/*************************************************************//**
Compare two tuples.
@return positive, 0, negative if a is greater, equal, less, than b,
respectively */
static MY_ATTRIBUTE((warn_unused_result))
int
row_merge_tuple_cmp(
/*================*/
	const dict_index_t*	index,	/*< in: index tree */
	ulint			n_uniq,	/*!< in: number of unique fields */
	ulint			n_field,/*!< in: number of fields */
	const mtuple_t&		a,	/*!< in: first tuple to be compared */
	const mtuple_t&		b,	/*!< in: second tuple to be compared */
	row_merge_dup_t*	dup)	/*!< in/out: for reporting duplicates,
					NULL if non-unique index */
{
	int		cmp;
	const dfield_t*	af	= a.fields;
	const dfield_t*	bf	= b.fields;
	ulint		n	= n_uniq;
	const dict_field_t* f = index->fields;

	ut_ad(n_uniq > 0);
	ut_ad(n_uniq <= n_field);

	/* Compare the fields of the tuples until a difference is
	found or we run out of fields to compare.  If !cmp at the
	end, the tuples are equal. */
	do {
		cmp = cmp_dfield_dfield(af++, bf++, (f++)->descending);
	} while (!cmp && --n);

	if (cmp) {
		return(cmp);
	}

	if (dup) {
		/* Report a duplicate value error if the tuples are
		logically equal.  NULL columns are logically inequal,
		although they are equal in the sorting order.  Find
		out if any of the fields are NULL. */
		for (const dfield_t* df = a.fields; df != af; df++) {
			if (dfield_is_null(df)) {
				goto no_report;
			}
		}

		row_merge_dup_report(dup, a.fields);
	}

no_report:
	/* The n_uniq fields were equal, but we compare all fields so
	that we will get the same (internal) order as in the B-tree. */
	for (n = n_field - n_uniq + 1; --n; ) {
		cmp = cmp_dfield_dfield(af++, bf++, (f++)->descending);
		if (cmp) {
			return(cmp);
		}
	}

	/* This should never be reached, except in a secondary index
	when creating a secondary index and a PRIMARY KEY, and there
	is a duplicate in the PRIMARY KEY that has not been detected
	yet. Internally, an index must never contain duplicates. */
	return(cmp);
}

/** Wrapper for row_merge_tuple_sort() to inject some more context to
UT_SORT_FUNCTION_BODY().
@param tuples array of tuples that being sorted
@param aux work area, same size as tuples[]
@param low lower bound of the sorting area, inclusive
@param high upper bound of the sorting area, inclusive */
#define row_merge_tuple_sort_ctx(tuples, aux, low, high)		\
	row_merge_tuple_sort(index,n_uniq,n_field,dup, tuples, aux, low, high)
/** Wrapper for row_merge_tuple_cmp() to inject some more context to
UT_SORT_FUNCTION_BODY().
@param a first tuple to be compared
@param b second tuple to be compared
@return positive, 0, negative, if a is greater, equal, less, than b,
respectively */
#define row_merge_tuple_cmp_ctx(a,b)			\
	row_merge_tuple_cmp(index, n_uniq, n_field, a, b, dup)

/**********************************************************************//**
Merge sort the tuple buffer in main memory. */
static
void
row_merge_tuple_sort(
/*=================*/
	const dict_index_t*	index,	/*!< in: index tree */
	ulint			n_uniq,	/*!< in: number of unique fields */
	ulint			n_field,/*!< in: number of fields */
	row_merge_dup_t*	dup,	/*!< in/out: reporter of duplicates
					(NULL if non-unique index) */
	mtuple_t*		tuples,	/*!< in/out: tuples */
	mtuple_t*		aux,	/*!< in/out: work area */
	ulint			low,	/*!< in: lower bound of the
					sorting area, inclusive */
	ulint			high)	/*!< in: upper bound of the
					sorting area, exclusive */
{
	ut_ad(n_field > 0);
	ut_ad(n_uniq <= n_field);

	UT_SORT_FUNCTION_BODY(row_merge_tuple_sort_ctx,
			      tuples, aux, low, high, row_merge_tuple_cmp_ctx);
}

/******************************************************//**
Sort a buffer. */
void
row_merge_buf_sort(
/*===============*/
	row_merge_buf_t*	buf,	/*!< in/out: sort buffer */
	row_merge_dup_t*	dup)	/*!< in/out: reporter of duplicates
					(NULL if non-unique index) */
{
  ut_ad(!buf->index->is_spatial());
  row_merge_tuple_sort(buf->index, buf->index->n_uniq, buf->index->n_fields,
                       dup, buf->tuples, buf->tmp_tuples, 0, buf->n_tuples);
}

/** Write the blob field data to temporary file and fill the offset,
length in the field data
@param	field		tuple field
@param	blob_file	file to store the blob data
@param	heap		heap to store the blob offset and length
@return DB_SUCCESS if successful */
static dberr_t row_merge_write_blob_to_tmp_file(
   dfield_t *field, uint32_t local_len,
   merge_file_t *blob_file, mem_heap_t **heap)
{
  if (blob_file->fd == OS_FILE_CLOSED)
  {
    blob_file->fd= row_merge_file_create_low(nullptr);
    if (blob_file->fd == OS_FILE_CLOSED)
      return DB_OUT_OF_MEMORY;
  }
  uint64_t val= blob_file->offset;
  uint32_t field_len= field->len;
  dberr_t err= os_file_write(
    IORequestWrite, "(bulk insert)", blob_file->fd,
    static_cast<const byte*>(field->data) + local_len, blob_file->offset,
    field_len - local_len);

  if (err != DB_SUCCESS)
    return err;

  byte *data=
    static_cast<byte*>(mem_heap_alloc(*heap,
                                      local_len + BTR_EXTERN_FIELD_REF_SIZE));

  memcpy(data, field->data, local_len);
  dfield_set_data(field, data, local_len + BTR_EXTERN_FIELD_REF_SIZE);
  dfield_set_ext(field);

  data+= local_len;
  /* Write zeroes for first 8 bytes */
  memset(data, 0, 8);
  /* Write offset for next 8 bytes */
  mach_write_to_8(data + 8, val);
  /* Write length of the blob in 4 bytes */
  mach_write_to_4(data + 16, field_len - local_len);
  blob_file->offset+= (field_len - local_len);
  blob_file->n_rec++;
  return err;
}

/** Variable length field data or fixed length
character greater than 255 can be stored externally. Instead
of storing it externally, InnoDB should store it in temporary
file and write offset, length into the tuple field
@param fields    index fields to be encode the blob
@param blob_file file to store the blob data
@param index     index for the tuple to be stored
@param heap      heap to store the blob offset and blob length
@return error code */
static
dberr_t row_merge_buf_blob(dfield_t *fields,
                           merge_file_t *blob_file,
                           const dict_index_t *index,
                           mem_heap_t **heap)
{
  const uint blob_prefix= dict_table_has_atomic_blobs(index->table)
    ? 0
    : REC_ANTELOPE_MAX_INDEX_COL_LEN;
  const uint min_local_len = blob_prefix
    ? blob_prefix + FIELD_REF_SIZE
    : 2 * FIELD_REF_SIZE;

  for (ulint i= index->first_user_field(); i < index->n_fields; i++)
  {
    dfield_t *field= &fields[i];
    if (dfield_is_null(field) || index->fields[i].fixed_len)
      continue;

    if (field->len > min_local_len &&
        DATA_BIG_COL(index->fields[i].col))
    {
      if (*heap == nullptr)
        *heap= mem_heap_create(256);
      dberr_t err=
        row_merge_write_blob_to_tmp_file(field, blob_prefix,
                                         blob_file, heap);
      if (err)
        return err;
     }
  }

  return DB_SUCCESS;
}

/** Write a buffer to a block.
@param buf              sorted buffer
@param block            buffer for writing to file
@param blob_file        blob file handle for doing bulk insert operation */
dberr_t row_merge_buf_write(const row_merge_buf_t *buf,
#ifndef DBUG_OFF
                            const merge_file_t *of, /*!< output file */
#endif
                            row_merge_block_t *block,
                            merge_file_t *blob_file)
{
	const dict_index_t*	index	= buf->index;
	ulint			n_fields= dict_index_get_n_fields(index);
	byte*			b	= &block[0];
	mem_heap_t*		blob_heap = nullptr;
	dberr_t			err = DB_SUCCESS;

	DBUG_ENTER("row_merge_buf_write");

	for (ulint i = 0; i < buf->n_tuples; i++) {
		const mtuple_t*	entry	= &buf->tuples[i];

		if (blob_file) {
			ut_ad(buf->index->is_primary());

			err = row_merge_buf_blob(entry->fields,
						 blob_file, buf->index,
                                                 &blob_heap);
			if (err != DB_SUCCESS) {
				goto func_exit;
			}
		}

		ulint rec_size= row_merge_buf_encode(
				&b, index, entry, n_fields);
		if (blob_file) {
			ulint rec_max_size =
				(srv_page_size == UNIV_PAGE_SIZE_MAX)
				? REDUNDANT_REC_MAX_DATA_SIZE
				: page_get_free_space_of_empty(
					dict_table_is_comp(
						index->table)) / 2;
			if (rec_size > rec_max_size) {
				err = DB_TOO_BIG_RECORD;
				goto func_exit;
			}
		}

		ut_ad(b < &block[srv_sort_buf_size]);

		DBUG_LOG("ib_merge_sort",
			 reinterpret_cast<const void*>(b) << ','
			 << of->fd << ',' << of->offset << ' ' <<
			 i << ": " <<
			 rec_printer(entry->fields, n_fields).str());
	}

	/* Write an "end-of-chunk" marker. */
	ut_a(b < &block[srv_sort_buf_size]);
	ut_a(b == &block[0] + buf->total_size || blob_file);
	*b++ = 0;
#ifdef HAVE_valgrind
	/* The rest of the block is uninitialized.  Initialize it
	to avoid bogus warnings. */
	memset(b, 0xff, &block[srv_sort_buf_size] - b);
#endif /* HAVE_valgrind */
	DBUG_LOG("ib_merge_sort",
		 "write " << reinterpret_cast<const void*>(b) << ','
		 << of->fd << ',' << of->offset << " EOF");
func_exit:
	if (blob_heap) {
		mem_heap_free(blob_heap);
	}

	DBUG_RETURN(err);
}

/******************************************************//**
Create a memory heap and allocate space for row_merge_rec_offsets()
and mrec_buf_t[3].
@return memory heap */
static
mem_heap_t*
row_merge_heap_create(
/*==================*/
	const dict_index_t*	index,		/*!< in: record descriptor */
	mrec_buf_t**		buf,		/*!< out: 3 buffers */
	rec_offs**		offsets1,	/*!< out: offsets */
	rec_offs**		offsets2)	/*!< out: offsets */
{
	ulint		i	= 1 + REC_OFFS_HEADER_SIZE
		+ dict_index_get_n_fields(index);
	mem_heap_t*	heap	= mem_heap_create(2 * i * sizeof **offsets1
						  + 3 * sizeof **buf);

	*buf = static_cast<mrec_buf_t*>(
		mem_heap_alloc(heap, 3 * sizeof **buf));
	*offsets1 = static_cast<rec_offs*>(
		mem_heap_alloc(heap, i * sizeof **offsets1));
	*offsets2 = static_cast<rec_offs*>(
		mem_heap_alloc(heap, i * sizeof **offsets2));

	rec_offs_set_n_alloc(*offsets1, i);
	rec_offs_set_n_alloc(*offsets2, i);
	rec_offs_set_n_fields(*offsets1, dict_index_get_n_fields(index));
	rec_offs_set_n_fields(*offsets2, dict_index_get_n_fields(index));

	return(heap);
}

/** Read a merge block from the file system.
@return whether the request was completed successfully */
bool
row_merge_read(
/*===========*/
	const pfs_os_file_t&	fd,	/*!< in: file descriptor */
	ulint			offset,	/*!< in: offset where to read
					in number of row_merge_block_t
					elements */
	row_merge_block_t*	buf,	/*!< out: data */
	row_merge_block_t*	crypt_buf, /*!< in: crypt buf or NULL */
	ulint			space)		/*!< in: space id */
{
	os_offset_t	ofs = ((os_offset_t) offset) * srv_sort_buf_size;

	DBUG_ENTER("row_merge_read");
	DBUG_LOG("ib_merge_sort", "fd=" << fd << " ofs=" << ofs);
	DBUG_EXECUTE_IF("row_merge_read_failure", DBUG_RETURN(FALSE););

	const dberr_t err = os_file_read(
		IORequestRead, fd, buf, ofs, srv_sort_buf_size, nullptr);

	/* If encryption is enabled decrypt buffer */
	if (err == DB_SUCCESS && srv_encrypt_log) {
		if (!log_tmp_block_decrypt(buf, srv_sort_buf_size,
					   crypt_buf, ofs)) {
			DBUG_RETURN(false);
		}

		srv_stats.n_merge_blocks_decrypted.inc();
		memcpy(buf, crypt_buf, srv_sort_buf_size);
	}

#ifdef POSIX_FADV_DONTNEED
	/* Each block is read exactly once.  Free up the file cache. */
	posix_fadvise(fd, ofs, srv_sort_buf_size, POSIX_FADV_DONTNEED);
#endif /* POSIX_FADV_DONTNEED */

	DBUG_RETURN(err == DB_SUCCESS);
}

/********************************************************************//**
Write a merge block to the file system.
@return whether the request was completed successfully
@retval	false	on error
@retval	true	on success */
bool
row_merge_write(
	const pfs_os_file_t&	fd,			/*!< in: file descriptor */
	ulint		offset,			/*!< in: offset where to write,
						in number of row_merge_block_t elements */
	const void*	buf,			/*!< in: data */
	void*		crypt_buf,		/*!< in: crypt buf or NULL */
	ulint		space)			/*!< in: space id */
{
	size_t		buf_len = srv_sort_buf_size;
	os_offset_t	ofs = buf_len * (os_offset_t) offset;
	void*		out_buf = (void *)buf;

	DBUG_ENTER("row_merge_write");
	DBUG_LOG("ib_merge_sort", "fd=" << fd << " ofs=" << ofs);
	DBUG_EXECUTE_IF("row_merge_write_failure", DBUG_RETURN(FALSE););

	/* For encrypted tables, encrypt data before writing */
	if (srv_encrypt_log) {
		if (!log_tmp_block_encrypt(static_cast<const byte*>(buf),
					   buf_len,
					   static_cast<byte*>(crypt_buf),
					   ofs)) {
			DBUG_RETURN(false);
		}

		srv_stats.n_merge_blocks_encrypted.inc();
		out_buf = crypt_buf;
	}

	const bool	success = DB_SUCCESS == os_file_write(
		IORequestWrite, "(merge)", fd, out_buf, ofs, buf_len);

#ifdef POSIX_FADV_DONTNEED
	/* The block will be needed on the next merge pass,
	but it can be evicted from the file cache meanwhile. */
	posix_fadvise(fd, ofs, buf_len, POSIX_FADV_DONTNEED);
#endif /* POSIX_FADV_DONTNEED */

	DBUG_RETURN(success);
}

/********************************************************************//**
Read a merge record.
@return pointer to next record, or NULL on I/O error or end of list */
const byte*
row_merge_read_rec(
/*===============*/
	row_merge_block_t*	block,	/*!< in/out: file buffer */
	mrec_buf_t*		buf,	/*!< in/out: secondary buffer */
	const byte*		b,	/*!< in: pointer to record */
	const dict_index_t*	index,	/*!< in: index of the record */
	const pfs_os_file_t&	fd,	/*!< in: file descriptor */
	ulint*			foffs,	/*!< in/out: file offset */
	const mrec_t**		mrec,	/*!< out: pointer to merge record,
					or NULL on end of list
					(non-NULL on I/O error) */
	rec_offs*		offsets,/*!< out: offsets of mrec */
	row_merge_block_t*	crypt_block, /*!< in: crypt buf or NULL */
	ulint			space) /*!< in: space id */
{
	ulint	extra_size;
	ulint	data_size;
	ulint	avail_size;

	ut_ad(b >= &block[0]);
	ut_ad(b < &block[srv_sort_buf_size]);

	ut_ad(rec_offs_get_n_alloc(offsets) == 1 + REC_OFFS_HEADER_SIZE
	      + dict_index_get_n_fields(index));

	DBUG_ENTER("row_merge_read_rec");

	extra_size = *b++;

	if (UNIV_UNLIKELY(!extra_size)) {
		/* End of list */
		*mrec = NULL;
		DBUG_LOG("ib_merge_sort",
			 "read " << reinterpret_cast<const void*>(b) << ',' <<
			 reinterpret_cast<const void*>(block) << ',' <<
			 fd << ',' << *foffs << " EOF");
		DBUG_RETURN(NULL);
	}

	if (extra_size >= 0x80) {
		/* Read another byte of extra_size. */

		if (UNIV_UNLIKELY(b >= &block[srv_sort_buf_size])) {
			if (!row_merge_read(fd, ++(*foffs), block,
					    crypt_block,
					    space)) {
err_exit:
				/* Signal I/O error. */
				*mrec = b;
				DBUG_RETURN(NULL);
			}

			/* Wrap around to the beginning of the buffer. */
			b = &block[0];
		}

		extra_size = (extra_size & 0x7f) << 8;
		extra_size |= *b++;
	}

	/* Normalize extra_size.  Above, value 0 signals "end of list". */
	extra_size--;

	/* Read the extra bytes. */

	if (UNIV_UNLIKELY(b + extra_size >= &block[srv_sort_buf_size])) {
		/* The record spans two blocks.  Copy the entire record
		to the auxiliary buffer and handle this as a special
		case. */

		avail_size = ulint(&block[srv_sort_buf_size] - b);
		ut_ad(avail_size < sizeof *buf);
		memcpy(*buf, b, avail_size);

		if (!row_merge_read(fd, ++(*foffs), block,
				    crypt_block,
				    space)) {

			goto err_exit;
		}

		/* Wrap around to the beginning of the buffer. */
		b = &block[0];

		/* Copy the record. */
		memcpy(*buf + avail_size, b, extra_size - avail_size);
		b += extra_size - avail_size;

		*mrec = *buf + extra_size;

		rec_init_offsets_temp(*mrec, index, offsets);

		data_size = rec_offs_data_size(offsets);

		/* These overflows should be impossible given that
		records are much smaller than either buffer, and
		the record starts near the beginning of each buffer. */
		ut_a(extra_size + data_size < sizeof *buf);
		ut_a(b + data_size < &block[srv_sort_buf_size]);

		/* Copy the data bytes. */
		memcpy(*buf + extra_size, b, data_size);
		b += data_size;

		goto func_exit;
	}

	*mrec = b + extra_size;

	rec_init_offsets_temp(*mrec, index, offsets);

	data_size = rec_offs_data_size(offsets);
	ut_ad(extra_size + data_size < sizeof *buf);

	b += extra_size + data_size;

	if (UNIV_LIKELY(b < &block[srv_sort_buf_size])) {
		/* The record fits entirely in the block.
		This is the normal case. */
		goto func_exit;
	}

	/* The record spans two blocks.  Copy it to buf. */

	b -= extra_size + data_size;
	avail_size = ulint(&block[srv_sort_buf_size] - b);
	memcpy(*buf, b, avail_size);
	*mrec = *buf + extra_size;

	rec_init_offsets_temp(*mrec, index, offsets);

	if (!row_merge_read(fd, ++(*foffs), block,
			    crypt_block,
			    space)) {

		goto err_exit;
	}

	/* Wrap around to the beginning of the buffer. */
	b = &block[0];

	/* Copy the rest of the record. */
	memcpy(*buf + avail_size, b, extra_size + data_size - avail_size);
	b += extra_size + data_size - avail_size;

func_exit:
	DBUG_LOG("ib_merge_sort",
		 reinterpret_cast<const void*>(b) << ',' <<
		 reinterpret_cast<const void*>(block)
		 << ",fd=" << fd << ',' << *foffs << ": "
		 << rec_printer(*mrec, 0, offsets).str());
	DBUG_RETURN(b);
}

/********************************************************************//**
Write a merge record. */
static
void
row_merge_write_rec_low(
/*====================*/
	byte*		b,	/*!< out: buffer */
	ulint		e,	/*!< in: encoded extra_size */
#ifndef DBUG_OFF
	ulint		size,	/*!< in: total size to write */
	const pfs_os_file_t&	fd,	/*!< in: file descriptor */
	ulint		foffs,	/*!< in: file offset */
#endif /* !DBUG_OFF */
	const mrec_t*	mrec,	/*!< in: record to write */
	const rec_offs*	offsets)/*!< in: offsets of mrec */
#ifdef DBUG_OFF
# define row_merge_write_rec_low(b, e, size, fd, foffs, mrec, offsets)	\
	row_merge_write_rec_low(b, e, mrec, offsets)
#endif /* DBUG_OFF */
{
	DBUG_ENTER("row_merge_write_rec_low");

#ifndef DBUG_OFF
	const byte* const end = b + size;
#endif /* DBUG_OFF */
	DBUG_ASSERT(e == rec_offs_extra_size(offsets) + 1);

	DBUG_LOG("ib_merge_sort",
		 reinterpret_cast<const void*>(b) << ",fd=" << fd << ','
		 << foffs << ": " << rec_printer(mrec, 0, offsets).str());

	if (e < 0x80) {
		*b++ = (byte) e;
	} else {
		*b++ = (byte) (0x80 | (e >> 8));
		*b++ = (byte) e;
	}

	memcpy(b, mrec - rec_offs_extra_size(offsets), rec_offs_size(offsets));
	DBUG_SLOW_ASSERT(b + rec_offs_size(offsets) == end);
	DBUG_VOID_RETURN;
}

/********************************************************************//**
Write a merge record.
@return pointer to end of block, or NULL on error */
static
byte*
row_merge_write_rec(
/*================*/
	row_merge_block_t*	block,	/*!< in/out: file buffer */
	mrec_buf_t*		buf,	/*!< in/out: secondary buffer */
	byte*			b,	/*!< in: pointer to end of block */
	const pfs_os_file_t&	fd,	/*!< in: file descriptor */
	ulint*			foffs,	/*!< in/out: file offset */
	const mrec_t*		mrec,	/*!< in: record to write */
	const rec_offs*         offsets,/*!< in: offsets of mrec */
	row_merge_block_t*	crypt_block, /*!< in: crypt buf or NULL */
	ulint			space)	   /*!< in: space id */
{
	ulint	extra_size;
	ulint	size;
	ulint	avail_size;

	ut_ad(block);
	ut_ad(buf);
	ut_ad(b >= &block[0]);
	ut_ad(b < &block[srv_sort_buf_size]);
	ut_ad(mrec);
	ut_ad(foffs);
	ut_ad(mrec < &block[0] || mrec > &block[srv_sort_buf_size]);
	ut_ad(mrec < buf[0] || mrec > buf[1]);

	/* Normalize extra_size.  Value 0 signals "end of list". */
	extra_size = rec_offs_extra_size(offsets) + 1;

	size = extra_size + (extra_size >= 0x80)
		+ rec_offs_data_size(offsets);

	if (UNIV_UNLIKELY(b + size >= &block[srv_sort_buf_size])) {
		/* The record spans two blocks.
		Copy it to the temporary buffer first. */
		avail_size = ulint(&block[srv_sort_buf_size] - b);

		row_merge_write_rec_low(buf[0],
					extra_size, size, fd, *foffs,
					mrec, offsets);

		/* Copy the head of the temporary buffer, write
		the completed block, and copy the tail of the
		record to the head of the new block. */
		memcpy(b, buf[0], avail_size);

		if (!row_merge_write(fd, (*foffs)++, block,
				     crypt_block,
				     space)) {
			return(NULL);
		}

		MEM_UNDEFINED(&block[0], srv_sort_buf_size);

		/* Copy the rest. */
		b = &block[0];
		memcpy(b, buf[0] + avail_size, size - avail_size);
		b += size - avail_size;
	} else {
		row_merge_write_rec_low(b, extra_size, size, fd, *foffs,
					mrec, offsets);
		b += size;
	}

	return(b);
}

/********************************************************************//**
Write an end-of-list marker.
@return pointer to end of block, or NULL on error */
static
byte*
row_merge_write_eof(
/*================*/
	row_merge_block_t*	block,		/*!< in/out: file buffer */
	byte*			b,		/*!< in: pointer to end of block */
	const pfs_os_file_t&	fd,		/*!< in: file descriptor */
	ulint*			foffs,		/*!< in/out: file offset */
	row_merge_block_t*	crypt_block, 	/*!< in: crypt buf or NULL */
	ulint			space)	   	/*!< in: space id */
{
	ut_ad(block);
	ut_ad(b >= &block[0]);
	ut_ad(b < &block[srv_sort_buf_size]);
	ut_ad(foffs);

	DBUG_ENTER("row_merge_write_eof");
	DBUG_LOG("ib_merge_sort",
		 reinterpret_cast<const void*>(b) << ',' <<
		 reinterpret_cast<const void*>(block) <<
		 ",fd=" << fd << ',' << *foffs);

	*b++ = 0;
	MEM_CHECK_DEFINED(&block[0], b - &block[0]);
	MEM_CHECK_ADDRESSABLE(&block[0], srv_sort_buf_size);

	/* The rest of the block is uninitialized. Silence warnings. */
	MEM_MAKE_DEFINED(b, &block[srv_sort_buf_size] - b);

	if (!row_merge_write(fd, (*foffs)++, block, crypt_block, space)) {
		DBUG_RETURN(NULL);
	}

	MEM_UNDEFINED(&block[0], srv_sort_buf_size);
	DBUG_RETURN(&block[0]);
}

/** Create a temporary file if it has not been created already.
@param[in,out]	tmpfd	temporary file handle
@param[in]	path	location for creating temporary file
@return true on success, false on error */
static MY_ATTRIBUTE((warn_unused_result))
bool
row_merge_tmpfile_if_needed(
	pfs_os_file_t*		tmpfd,
	const char*	path)
{
	if (*tmpfd == OS_FILE_CLOSED) {
		*tmpfd = row_merge_file_create_low(path);
		if (*tmpfd != OS_FILE_CLOSED) {
			MONITOR_ATOMIC_INC(MONITOR_ALTER_TABLE_SORT_FILES);
		}
	}

	return(*tmpfd != OS_FILE_CLOSED);
}

/** Create a temporary file for merge sort if it was not created already.
@param[in,out]	file	merge file structure
@param[in]	nrec	number of records in the file
@param[in]	path	location for creating temporary file
@return  true on success, false on error */
static MY_ATTRIBUTE((warn_unused_result))
bool
row_merge_file_create_if_needed(
	merge_file_t*	file,
	pfs_os_file_t*	tmpfd,
	ulint		nrec,
	const char*	path)
{
	ut_ad(file->fd == OS_FILE_CLOSED || *tmpfd != OS_FILE_CLOSED);
	if (file->fd == OS_FILE_CLOSED && row_merge_file_create(file, path)!= OS_FILE_CLOSED) {
		MONITOR_ATOMIC_INC(MONITOR_ALTER_TABLE_SORT_FILES);
		if (!row_merge_tmpfile_if_needed(tmpfd, path) ) {
			return(false);
		}

		file->n_rec = nrec;
	}

	ut_ad(file->fd == OS_FILE_CLOSED || *tmpfd != OS_FILE_CLOSED);
	return(file->fd != OS_FILE_CLOSED);
}

/** Copy the merge data tuple from another merge data tuple.
@param[in]	mtuple		source merge data tuple
@param[in,out]	prev_mtuple	destination merge data tuple
@param[in]	n_unique	number of unique fields exist in the mtuple
@param[in,out]	heap		memory heap where last_mtuple allocated */
static
void
row_mtuple_create(
	const mtuple_t*	mtuple,
	mtuple_t*	prev_mtuple,
	ulint		n_unique,
	mem_heap_t*	heap)
{
	memcpy(prev_mtuple->fields, mtuple->fields,
	       n_unique * sizeof *mtuple->fields);

	dfield_t*	field = prev_mtuple->fields;

	for (ulint i = 0; i < n_unique; i++) {
		dfield_dup(field++, heap);
	}
}

/** Compare two merge data tuples.
@param[in]	prev_mtuple	merge data tuple
@param[in]	current_mtuple	merge data tuple
@param[in,out]	dup		reporter of duplicates
@retval positive, 0, negative if current_mtuple is greater, equal, less, than
last_mtuple. */
static
int
row_mtuple_cmp(
	const mtuple_t*		prev_mtuple,
	const mtuple_t*		current_mtuple,
	row_merge_dup_t*	dup)
{
  ut_ad(dup->index->is_primary());
  const ulint n_uniq= dup->index->n_uniq;
  return row_merge_tuple_cmp(dup->index, n_uniq, n_uniq,
                             *current_mtuple, *prev_mtuple, dup);
}

/** Insert cached spatial index rows.
@param[in]	trx_id		transaction id
@param[in]	sp_tuples	cached spatial rows
@param[in]	num_spatial	number of spatial indexes
@param[in,out]	heap		temporary memory heap
@param[in,out]	pcur		cluster index cursor
@param[in,out]	started		whether mtr is active
@param[in,out]	mtr		mini-transaction
@return DB_SUCCESS or error number */
static
dberr_t
row_merge_spatial_rows(
	trx_id_t		trx_id,
	spatial_index_info**	sp_tuples,
	ulint			num_spatial,
	mem_heap_t*		heap,
	btr_pcur_t*		pcur,
	bool&			started,
	mtr_t*			mtr)
{
  if (!sp_tuples)
    return DB_SUCCESS;

  for (ulint j= 0; j < num_spatial; j++)
    if (dberr_t err= sp_tuples[j]->insert(trx_id, pcur, started, heap, mtr))
      return err;

  mem_heap_empty(heap);
  return DB_SUCCESS;
}

/** Check if the geometry field is valid.
@param[in]	row		the row
@param[in]	index		spatial index
@return true if it's valid, false if it's invalid. */
static
bool
row_geo_field_is_valid(
	const dtuple_t*		row,
	dict_index_t*		index)
{
	const dict_field_t*	ind_field
		= dict_index_get_nth_field(index, 0);
	const dict_col_t*	col
		= ind_field->col;
	ulint			col_no
		= dict_col_get_no(col);
	const dfield_t*		dfield
		= dtuple_get_nth_field(row, col_no);

	if (dfield_is_null(dfield)
	    || dfield_get_len(dfield) < GEO_DATA_HEADER_SIZE) {
		return(false);
	}

	return(true);
}

/** Reads clustered index of the table and create temporary files
containing the index entries for the indexes to be built.
@param[in]	trx		transaction
@param[in,out]	table		MySQL table object, for reporting erroneous
				records
@param[in]	old_table	table where rows are read from
@param[in]	new_table	table where indexes are created; identical to
				old_table unless creating a PRIMARY KEY
@param[in]	online		true if creating indexes online
@param[in]	index		indexes to be created
@param[in]	fts_sort_idx	full-text index to be created, or NULL
@param[in]	psort_info	parallel sort info for fts_sort_idx creation,
				or NULL
@param[in]	files		temporary files
@param[in]	key_numbers	MySQL key numbers to create
@param[in]	n_index		number of indexes to create
@param[in]	defaults	default values of added, changed columns, or NULL
@param[in]	add_v		newly added virtual columns along with indexes
@param[in]	col_map		mapping of old column numbers to new ones, or
NULL if old_table == new_table
@param[in]	add_autoinc	number of added AUTO_INCREMENT columns, or
ULINT_UNDEFINED if none is added
@param[in,out]	sequence	autoinc sequence
@param[in,out]	block		file buffer
@param[in]	skip_pk_sort	whether the new PRIMARY KEY will follow
existing order
@param[in,out]	tmpfd		temporary file handle
@param[in,out]	stage		performance schema accounting object, used by
ALTER TABLE. stage->n_pk_recs_inc() will be called for each record read and
stage->inc() will be called for each page read.
@param[in]	pct_cost	percent of task weight out of total alter job
@param[in,out]	crypt_block	crypted file buffer
@param[in]	eval_table	mysql table used to evaluate virtual column
				value, see innobase_get_computed_value().
@param[in]	allow_not_null	allow null to not-null conversion
@param[in]	col_collate	columns whose collations changed, or nullptr
@return DB_SUCCESS or error */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_merge_read_clustered_index(
	trx_t*			trx,
	struct TABLE*		table,
	const dict_table_t*	old_table,
	dict_table_t*		new_table,
	bool			online,
	dict_index_t**		index,
	dict_index_t*		fts_sort_idx,
	fts_psort_t*		psort_info,
	merge_file_t*		files,
	const ulint*		key_numbers,
	ulint			n_index,
	const dtuple_t*		defaults,
	const dict_add_v_col_t*	add_v,
	const ulint*		col_map,
	ulint			add_autoinc,
	ib_sequence_t&		sequence,
	row_merge_block_t*	block,
	bool			skip_pk_sort,
	pfs_os_file_t*		tmpfd,
	ut_stage_alter_t*	stage,
	double 			pct_cost,
	row_merge_block_t*	crypt_block,
	struct TABLE*		eval_table,
	bool			allow_not_null,
	const col_collations*	col_collate)
{
	dict_index_t*		clust_index;	/* Clustered index */
	mem_heap_t*		row_heap = NULL;/* Heap memory to create
						clustered index tuples */
	row_merge_buf_t**	merge_buf;	/* Temporary list for records*/
	mem_heap_t*		v_heap = NULL;	/* Heap memory to process large
						data for virtual column */
	btr_pcur_t		pcur;		/* Cursor on the clustered
						index */
	mtr_t			mtr;		/* Mini transaction */
	bool			mtr_started = false;
	dberr_t			err = DB_SUCCESS;/* Return code */
	ulint			n_nonnull = 0;	/* number of columns
						changed to NOT NULL */
	ulint*			nonnull = NULL;	/* NOT NULL columns */
	dict_index_t*		fts_index = NULL;/* FTS index */
	doc_id_t		doc_id = 0;
	doc_id_t		max_doc_id = 0;
	ibool			add_doc_id = FALSE;
	pthread_cond_t*		fts_parallel_sort_cond = nullptr;
	spatial_index_info**	sp_tuples = nullptr;
	ulint			num_spatial = 0;
	BtrBulk*		clust_btr_bulk = NULL;
	bool			clust_temp_file = false;
	mem_heap_t*		mtuple_heap = NULL;
	mtuple_t		prev_mtuple;
	mem_heap_t*		conv_heap = NULL;
	double 			curr_progress = 0.0;
	ib_uint64_t		read_rows = 0;
	ib_uint64_t		table_total_rows = 0;
	char			new_sys_trx_start[8];
	char			new_sys_trx_end[8];
	byte			any_autoinc_data[8] = {0};
	bool			vers_update_trt = false;

	DBUG_ENTER("row_merge_read_clustered_index");

	ut_ad((old_table == new_table) == !col_map);
	ut_ad(old_table->fts || !new_table->fts || !new_table->versioned());
	ut_ad(!defaults || col_map);
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
	ut_ad(trx->id);

	table_total_rows = dict_table_get_n_rows(old_table);
	if(table_total_rows == 0) {
		/* We don't know total row count */
		table_total_rows = 1;
	}

	trx->op_info = "reading clustered index";

#ifdef FTS_INTERNAL_DIAG_PRINT
	DEBUG_FTS_SORT_PRINT("FTS_SORT: Start Create Index\n");
#endif

	/* Create and initialize memory for record buffers */

	merge_buf = static_cast<row_merge_buf_t**>(
		ut_malloc_nokey(n_index * sizeof *merge_buf));

	row_merge_dup_t	clust_dup = {index[0], table, col_map, 0};
	dfield_t*	prev_fields = nullptr;
	const ulint	n_uniq = dict_index_get_n_unique(index[0]);

	ut_ad(trx->mysql_thd != NULL);

	const char*	path = thd_innodb_tmpdir(trx->mysql_thd);

	ut_ad(!skip_pk_sort || dict_index_is_clust(index[0]));
	/* There is no previous tuple yet. */
	prev_mtuple.fields = NULL;

	for (ulint i = 0; i < n_index; i++) {
		if (index[i]->type & DICT_FTS) {

			/* We are building a FT index, make sure
			we have the temporary 'fts_sort_idx' */
			ut_a(fts_sort_idx);

			fts_index = index[i];

			merge_buf[i] = row_merge_buf_create(fts_sort_idx);

			add_doc_id = DICT_TF2_FLAG_IS_SET(
				new_table, DICT_TF2_FTS_ADD_DOC_ID);

			/* If Doc ID does not exist in the table itself,
			fetch the first FTS Doc ID */
			if (add_doc_id) {
				fts_get_next_doc_id(
					(dict_table_t*) new_table,
					&doc_id);
				ut_ad(doc_id > 0);
			}

			row_fts_start_psort(psort_info);
			fts_parallel_sort_cond =
				 &psort_info[0].psort_common->sort_cond;
		} else {
			if (dict_index_is_spatial(index[i])) {
				num_spatial++;
			}

			merge_buf[i] = row_merge_buf_create(index[i]);
		}
	}

	if (num_spatial > 0) {
		ulint	count = 0;

		sp_tuples = static_cast<spatial_index_info**>(
			ut_malloc_nokey(num_spatial
					* sizeof(*sp_tuples)));

		for (ulint i = 0; i < n_index; i++) {
			if (dict_index_is_spatial(index[i])) {
				sp_tuples[count]
					= UT_NEW_NOKEY(
						spatial_index_info(index[i]));
				count++;
			}
		}

		ut_ad(count == num_spatial);
	}

	mtr.start();
	mtr_started = true;

	/* Find the clustered index and create a persistent cursor
	based on that. */

	clust_index = dict_table_get_first_index(old_table);
	const ulint old_trx_id_col = ulint(old_table->n_cols)
		- (DATA_N_SYS_COLS - DATA_TRX_ID);
	ut_ad(old_table->cols[old_trx_id_col].mtype == DATA_SYS);
	ut_ad(old_table->cols[old_trx_id_col].prtype
	      == (DATA_TRX_ID | DATA_NOT_NULL));
	ut_ad(old_table->cols[old_trx_id_col + 1].mtype == DATA_SYS);
	ut_ad(old_table->cols[old_trx_id_col + 1].prtype
	      == (DATA_ROLL_PTR | DATA_NOT_NULL));
	const ulint new_trx_id_col = col_map
		? col_map[old_trx_id_col] : old_trx_id_col;
	uint64_t n_rows = 0;

	err = pcur.open_leaf(true, clust_index, BTR_SEARCH_LEAF, &mtr);
	if (err != DB_SUCCESS) {
err_exit:
		trx->error_key_num = 0;
		goto func_exit;
	} else {
		const page_t* const page = btr_pcur_get_page(&pcur);
		const auto comp = page_is_comp(page);
		const rec_t* const rec = comp
			? page_rec_next_get<true>(page,
						  btr_pcur_get_rec(&pcur))
			: page_rec_next_get<false>(page,
						   btr_pcur_get_rec(&pcur));
		if (!rec) {
corrupted_metadata:
			err = DB_CORRUPTION;
			goto err_exit;
		}
		if (rec_get_info_bits(rec, comp) & REC_INFO_MIN_REC_FLAG) {
			if (!clust_index->is_instant()) {
				goto corrupted_metadata;
			}
			if (comp
			    && rec_get_status(rec) != REC_STATUS_INSTANT) {
				goto corrupted_metadata;
			}
			/* Skip the metadata pseudo-record. */
			btr_pcur_get_page_cur(&pcur)->rec =
				const_cast<rec_t*>(rec);
		} else if (clust_index->is_instant()) {
			goto corrupted_metadata;
		}
	}

	/* Check if the table is supposed to be empty for our read view.

	If we read bulk_trx_id as an older transaction ID, it is not
	incorrect to check here whether that transaction should be
	visible to us. If bulk_trx_id is not visible to us, the table
	must have been empty at an earlier point of time, also in our
	read view.

	An INSERT would only update bulk_trx_id in
	row_ins_clust_index_entry_low() if the table really was empty
	(everything had been purged), when holding a leaf page latch
	in the clustered index (actually, the root page is the only
	leaf page in that case).

	We are holding a clustered index leaf page latch here.
	That will obviously prevent any concurrent INSERT from
	updating bulk_trx_id while we read it. */
	if (!online) {
	} else if (trx_id_t bulk_trx_id = old_table->bulk_trx_id) {
		ut_ad(trx->read_view.is_open());
		ut_ad(bulk_trx_id != trx->id);
		if (!trx->read_view.changes_visible(bulk_trx_id)) {
			goto func_exit;
		}
	}

	if (old_table != new_table) {
		/* The table is being rebuilt.  Identify the columns
		that were flagged NOT NULL in the new table, so that
		we can quickly check that the records in the old table
		do not violate the added NOT NULL constraints. */

		nonnull = static_cast<ulint*>(
			ut_malloc_nokey(dict_table_get_n_cols(new_table)
				  * sizeof *nonnull));

		for (ulint i = 0; i < dict_table_get_n_cols(old_table); i++) {
			if (dict_table_get_nth_col(old_table, i)->prtype
			    & DATA_NOT_NULL) {
				continue;
			}

			const ulint j = col_map[i];

			if (j == ULINT_UNDEFINED) {
				/* The column was dropped. */
				continue;
			}

			if (dict_table_get_nth_col(new_table, j)->prtype
			    & DATA_NOT_NULL) {
				nonnull[n_nonnull++] = j;
			}
		}

		if (!n_nonnull) {
			ut_free(nonnull);
			nonnull = NULL;
		}
	}

	row_heap = mem_heap_create(sizeof(mrec_buf_t));

	if (dict_table_is_comp(old_table)
	    && !dict_table_is_comp(new_table)) {
		conv_heap = mem_heap_create(sizeof(mrec_buf_t));
	}

	if (skip_pk_sort) {
		prev_fields = static_cast<dfield_t*>(
			ut_malloc_nokey(n_uniq * sizeof *prev_fields));
		mtuple_heap = mem_heap_create(sizeof(mrec_buf_t));
	}

	mach_write_to_8(new_sys_trx_start, trx->id);
	mach_write_to_8(new_sys_trx_end, TRX_ID_MAX);

	/* Scan the clustered index. */
	for (;;) {
		/* Do not continue if table pages are still encrypted */
		if (!old_table->is_readable() || !new_table->is_readable()) {
			err = DB_DECRYPTION_FAILED;
			goto err_exit;
		}

		const rec_t*	rec;
		trx_id_t	rec_trx_id;
		rec_offs*	offsets;
		dtuple_t*	row;
		row_ext_t*	ext;
		page_cur_t*	cur	= btr_pcur_get_page_cur(&pcur);

		stage->n_pk_recs_inc();

		if (!page_cur_move_to_next(cur)) {
corrupted_rec:
			err = DB_CORRUPTION;
			goto err_exit;
		}

		if (page_cur_is_after_last(cur)) {

			stage->inc();

			if (UNIV_UNLIKELY(trx_is_interrupted(trx))) {
				err = DB_INTERRUPTED;
				goto err_exit;
			}

			if (online && old_table != new_table) {
				err = row_log_table_get_error(clust_index);
				if (err != DB_SUCCESS) {
					goto err_exit;
				}
			}

			/* Insert the cached spatial index rows. */
			err = row_merge_spatial_rows(
				trx->id, sp_tuples, num_spatial,
				row_heap, &pcur, mtr_started, &mtr);

			if (err != DB_SUCCESS) {
				goto func_exit;
			}

			mem_heap_empty(row_heap);

			if (!mtr_started) {
				ut_ad(!mtr.is_active());
				mtr.start();
				mtr_started = true;
				/* Restore position on the record, or its
				predecessor if the record was purged
				meanwhile. */
				if (pcur.restore_position(BTR_SEARCH_LEAF,
							  &mtr)
				    == btr_pcur_t::CORRUPTED) {
corrupted_index:
					err = DB_CORRUPTION;
					goto func_exit;
				}
				/* Move to the successor of the
				original record. */
				if (!btr_pcur_move_to_next_user_rec(
					    &pcur, &mtr)) {
end_of_index:
					row = NULL;
					mtr.commit();
					mtr_started = false;
					mem_heap_free(row_heap);
					row_heap = NULL;
					ut_free(nonnull);
					nonnull = NULL;
					goto write_buffers;
				}
			} else {
				uint32_t next_page_no = btr_page_get_next(
					page_cur_get_page(cur));

				if (next_page_no == FIL_NULL) {
					goto end_of_index;
				}

				buf_block_t* block = buf_page_get_gen(
					page_id_t(old_table->space->id,
						  next_page_no),
					old_table->space->zip_size(),
					RW_S_LATCH, nullptr, BUF_GET, &mtr,
					&err);
				if (!block) {
					goto err_exit;
				}

				buf_page_make_young_if_needed(&block->page);

				const auto s = mtr.get_savepoint();
				mtr.rollback_to_savepoint(s - 2, s - 1);

				page_cur_set_before_first(block, cur);
				if (!page_cur_move_to_next(cur)
				    || page_cur_is_after_last(cur)) {
					goto corrupted_rec;
				}
			}
		} else {
			mem_heap_empty(row_heap);
		}

		rec = page_cur_get_rec(cur);

		if (online) {
			offsets = rec_get_offsets(rec, clust_index, NULL,
						  clust_index->n_core_fields,
						  ULINT_UNDEFINED, &row_heap);
			rec_trx_id = row_get_rec_trx_id(rec, clust_index,
							offsets);

			/* Perform a REPEATABLE READ.

			When rebuilding the table online,
			row_log_table_apply() must not see a newer
			state of the table when applying the log.
			This is mainly to prevent false duplicate key
			errors, because the log will identify records
			by the PRIMARY KEY, and also to prevent unsafe
			BLOB access.

			When creating a secondary index online, this
			table scan must not see records that have only
			been inserted to the clustered index, but have
			not been written to the online_log of
			index[]. If we performed READ UNCOMMITTED, it
			could happen that the ADD INDEX reaches
			ONLINE_INDEX_COMPLETE state between the time
			the DML thread has updated the clustered index
			but has not yet accessed secondary index. */
			ut_ad(trx->read_view.is_open());
			ut_ad(rec_trx_id != trx->id);

			if (!trx->read_view.changes_visible(rec_trx_id)) {
				if (rec_trx_id
				    >= trx->read_view.low_limit_id()
				    && rec_trx_id
				    >= trx_sys.get_max_trx_id()) {
					goto corrupted_rec;
				}

				rec_t*	old_vers;

				row_vers_build_for_consistent_read(
					rec, &mtr, clust_index, &offsets,
					&trx->read_view, &row_heap,
					row_heap, &old_vers, NULL);

				if (!old_vers) {
					continue;
				}

				/* The old version must necessarily be
				in the "prehistory", because the
				exclusive lock in
				ha_innobase::prepare_inplace_alter_table()
				forced the completion of any transactions
				that accessed this table. */
				ut_ad(row_get_rec_trx_id(old_vers, clust_index,
							 offsets) < trx->id);

				rec = old_vers;
				rec_trx_id = 0;
			}

			if (rec_get_deleted_flag(
				    rec,
				    dict_table_is_comp(old_table))) {
				/* In delete-marked records, DB_TRX_ID must
				always refer to an existing undo log record.
				Above, we did reset rec_trx_id = 0
				for rec = old_vers.*/
				ut_ad(rec == page_cur_get_rec(cur)
				      ? rec_trx_id
				      : !rec_trx_id);
				/* This record was deleted in the latest
				committed version, or it was deleted and
				then reinserted-by-update before purge
				kicked in. Skip it. */
				continue;
			}

			ut_ad(!rec_offs_any_null_extern(rec, offsets));
		} else if (rec_get_deleted_flag(
				   rec, dict_table_is_comp(old_table))) {
			/* In delete-marked records, DB_TRX_ID must
			always refer to an existing undo log record. */
			ut_d(rec_trx_id = rec_get_trx_id(rec, clust_index));
			ut_ad(rec_trx_id);
			/* This must be a purgeable delete-marked record,
			and the transaction that delete-marked the record
			must have been committed before this
			!online ALTER TABLE transaction. */
			ut_ad(rec_trx_id < trx->id);
			/* Skip delete-marked records.

			Skipping delete-marked records will make the
			created indexes unuseable for transactions
			whose read views were created before the index
			creation completed, but an attempt to preserve
			the history would make it tricky to detect
			duplicate keys. */
			continue;
		} else {
			offsets = rec_get_offsets(rec, clust_index, NULL,
						  clust_index->n_core_fields,
						  ULINT_UNDEFINED, &row_heap);
			/* This is a locking ALTER TABLE.

			If we are not rebuilding the table, the
			DB_TRX_ID does not matter, as it is not being
			written to any secondary indexes; see
			if (old_table == new_table) below.

			If we are rebuilding the table, the
			DB_TRX_ID,DB_ROLL_PTR should be reset, because
			there will be no history available. */
			ut_ad(rec_get_trx_id(rec, clust_index) < trx->id);
			rec_trx_id = 0;
		}

		/* When !online, we are holding a lock on old_table, preventing
		any inserts that could have written a record 'stub' before
		writing out off-page columns. */
		ut_ad(!rec_offs_any_null_extern(rec, offsets));

		/* Build a row based on the clustered index. */

		row = row_build_w_add_vcol(ROW_COPY_POINTERS, clust_index,
					   rec, offsets, new_table,
					   defaults, add_v, col_map, &ext,
					   row_heap);
		ut_ad(row);

		for (ulint i = 0; i < n_nonnull; i++) {
			dfield_t*	field	= &row->fields[nonnull[i]];

			ut_ad(dfield_get_type(field)->prtype & DATA_NOT_NULL);

			if (dfield_is_null(field)) {

				Field* null_field =
					table->field[nonnull[i]];

				null_field->set_warning(
					Sql_condition::WARN_LEVEL_WARN,
					WARN_DATA_TRUNCATED, 1,
					ulong(n_rows + 1));

				if (!allow_not_null) {
					err = DB_INVALID_NULL;
					goto err_exit;
				}

				const dfield_t& default_field
					= defaults->fields[nonnull[i]];

				*field = default_field;
			}
		}

		/* Get the next Doc ID */
		if (add_doc_id) {
			doc_id++;
		} else {
			doc_id = 0;
		}

		ut_ad(row->fields[new_trx_id_col].type.mtype == DATA_SYS);
		ut_ad(row->fields[new_trx_id_col].type.prtype
		      == (DATA_TRX_ID | DATA_NOT_NULL));
		ut_ad(row->fields[new_trx_id_col].len == DATA_TRX_ID_LEN);
		ut_ad(row->fields[new_trx_id_col + 1].type.mtype == DATA_SYS);
		ut_ad(row->fields[new_trx_id_col + 1].type.prtype
		      == (DATA_ROLL_PTR | DATA_NOT_NULL));
		ut_ad(row->fields[new_trx_id_col + 1].len == DATA_ROLL_PTR_LEN);

		if (old_table == new_table) {
			/* Do not bother touching DB_TRX_ID,DB_ROLL_PTR
			because they are not going to be written into
			secondary indexes. */
		} else if (rec_trx_id < trx->id) {
			/* Reset the DB_TRX_ID,DB_ROLL_PTR of old rows
			for which history is not going to be
			available after the rebuild operation.
			This essentially mimics row_purge_reset_trx_id(). */
			row->fields[new_trx_id_col].data
				= const_cast<byte*>(reset_trx_id);
			row->fields[new_trx_id_col + 1].data
				= const_cast<byte*>(reset_trx_id
						    + DATA_TRX_ID_LEN);
		}

		if (add_autoinc != ULINT_UNDEFINED) {

			ut_ad(add_autoinc
			      < dict_table_get_n_user_cols(new_table));

			dfield_t* dfield = dtuple_get_nth_field(row,
								add_autoinc);

			if (new_table->versioned()) {
				if (dtuple_get_nth_field(row,
							 new_table->vers_end)
				    ->vers_history_row()) {
					if (dfield_get_type(dfield)->prtype & DATA_NOT_NULL) {
						err = DB_UNSUPPORTED;
						my_error(ER_UNSUPPORTED_EXTENSION, MYF(0),
							 old_table->name.m_name);
						goto func_exit;
					}
					dfield_set_null(dfield);
				} else {
					// set not null
					ulint len = dfield_get_type(dfield)->len;
					dfield_set_data(dfield, any_autoinc_data, len);
				}
			}

			if (dfield_is_null(dfield)) {
				goto write_buffers;
			}

			const dtype_t*  dtype = dfield_get_type(dfield);
			byte*	b = static_cast<byte*>(dfield_get_data(dfield));

			if (sequence.eof()) {
				ib_errf(trx->mysql_thd, IB_LOG_LEVEL_ERROR,
					ER_AUTOINC_READ_FAILED, "[NULL]");
				err = DB_ERROR;
				goto err_exit;
			}

			ulonglong	value = sequence++;

			switch (dtype_get_mtype(dtype)) {
			case DATA_INT: {
				ibool	usign;
				ulint	len = dfield_get_len(dfield);

				usign = dtype_get_prtype(dtype) & DATA_UNSIGNED;
				mach_write_ulonglong(b, value, len, usign);

				break;
				}

			case DATA_FLOAT:
				mach_float_write(
					b, static_cast<float>(value));
				break;

			case DATA_DOUBLE:
				mach_double_write(
					b, static_cast<double>(value));
				break;

			default:
				ut_ad(0);
			}
		}

		if (old_table->versioned()) {
			if (!new_table->versioned()
			    && clust_index->vers_history_row(rec, offsets)) {
				continue;
			}
		} else if (new_table->versioned()) {
			dfield_t* start =
			    dtuple_get_nth_field(row, new_table->vers_start);
			dfield_t* end =
			    dtuple_get_nth_field(row, new_table->vers_end);
			dfield_set_data(start, new_sys_trx_start, 8);
			dfield_set_data(end, new_sys_trx_end, 8);
			vers_update_trt = true;
		}

write_buffers:
		/* Build all entries for all the indexes to be created
		in a single scan of the clustered index. */

		n_rows++;
		ulint	s_idx_cnt = 0;
		bool	skip_sort = skip_pk_sort
			&& dict_index_is_clust(merge_buf[0]->index);

		for (ulint k = 0, i = 0; i < n_index; i++, skip_sort = false) {
			row_merge_buf_t*	buf	= merge_buf[i];
			ulint			rows_added = 0;

			if (dict_index_is_spatial(buf->index)) {
				if (!row) {
					continue;
				}

				ut_ad(sp_tuples[s_idx_cnt]->index
				      == buf->index);

				/* If the geometry field is invalid, report
				error. */
				if (!row_geo_field_is_valid(row, buf->index)) {
					err = DB_CANT_CREATE_GEOMETRY_OBJECT;
					trx->error_key_num = i;
					break;
				}

				sp_tuples[s_idx_cnt]->add(row, ext, buf->heap);
				s_idx_cnt++;

				continue;
			}

			ut_ad(!row
			      || !dict_index_is_clust(buf->index)
			      || trx_id_check(row->fields[new_trx_id_col].data,
					      trx->id));

			merge_file_t*	file = &files[k++];

			if (UNIV_LIKELY
			    (row && (rows_added = row_merge_buf_add(
					buf, fts_index, old_table, new_table,
					psort_info, row, ext,
					&doc_id, conv_heap, &err,
					&v_heap, eval_table, trx,
					col_collate)))) {

				/* If we are creating FTS index,
				a single row can generate more
				records for tokenized word */
				file->n_rec += rows_added;

				if (err != DB_SUCCESS) {
					ut_ad(err == DB_TOO_BIG_RECORD);
					break;
				}

				if (doc_id > max_doc_id) {
					max_doc_id = doc_id;
				}

				if (buf->index->type & DICT_FTS) {
					/* Check if error occurs in child thread */
					for (ulint j = 0;
					     j < fts_sort_pll_degree; j++) {
						if (psort_info[j].error
							!= DB_SUCCESS) {
							err = psort_info[j].error;
							trx->error_key_num = i;
							break;
						}
					}

					if (err != DB_SUCCESS) {
						break;
					}
				}

				if (skip_sort) {
					ut_ad(buf->n_tuples > 0);
					const mtuple_t*	curr =
						&buf->tuples[buf->n_tuples - 1];

					ut_ad(i == 0);
					ut_ad(dict_index_is_clust(merge_buf[0]->index));
					/* Detect duplicates by comparing the
					current record with previous record.
					When temp file is not used, records
					should be in sorted order. */
					if (prev_mtuple.fields != NULL
					    && (row_mtuple_cmp(
						&prev_mtuple, curr,
						&clust_dup) == 0)) {

						err = DB_DUPLICATE_KEY;
						trx->error_key_num
							= key_numbers[0];
						goto func_exit;
					}

					prev_mtuple.fields = curr->fields;
				}

				continue;
			}

			if (err == DB_COMPUTE_VALUE_FAILED) {
				trx->error_key_num = i;
				goto func_exit;
			}

			if (buf->index->type & DICT_FTS) {
				if (!row || !doc_id) {
					continue;
				}
			}

			/* The buffer must be sufficiently large
			to hold at least one record. It may only
			be empty when we reach the end of the
			clustered index. row_merge_buf_add()
			must not have been called in this loop. */
			ut_ad(buf->n_tuples || row == NULL);

			/* We have enough data tuples to form a block.
			Sort them and write to disk if temp file is used
			or insert into index if temp file is not used. */
			ut_ad(old_table == new_table
			      ? !dict_index_is_clust(buf->index)
			      : (i == 0) == dict_index_is_clust(buf->index));

			/* We have enough data tuples to form a block.
			Sort them (if !skip_sort) and write to disk. */

			if (buf->n_tuples) {
				if (skip_sort) {
					/* Temporary File is not used.
					so insert sorted block to the index */
					if (row != NULL) {
						/* We have to do insert the
						cached spatial index rows, since
						after the mtr_commit, the cluster
						index page could be updated, then
						the data in cached rows become
						invalid. */
						err = row_merge_spatial_rows(
							trx->id, sp_tuples,
							num_spatial,
							row_heap,
							&pcur, mtr_started,
							&mtr);

						if (err != DB_SUCCESS) {
							goto func_exit;
						}

						/* We are not at the end of
						the scan yet. We must
						mtr.commit() in order to be
						able to call log_free_check()
						in row_merge_insert_index_tuples().
						Due to mtr.commit(), the
						current row will be invalid, and
						we must reread it on the next
						loop iteration. */
						if (mtr_started) {
							if (!btr_pcur_move_to_prev_on_page(&pcur)) {
								err = DB_CORRUPTION;
								goto func_exit;
							}
							btr_pcur_store_position(
								&pcur, &mtr);

							mtr.commit();
							mtr_started = false;
						}
					}

					mem_heap_empty(mtuple_heap);
					prev_mtuple.fields = prev_fields;

					row_mtuple_create(
						&buf->tuples[buf->n_tuples - 1],
						&prev_mtuple, n_uniq,
						mtuple_heap);

					if (clust_btr_bulk == NULL) {
						clust_btr_bulk = UT_NEW_NOKEY(
							BtrBulk(index[i],
								trx));
					} else {
						clust_btr_bulk->latch();
					}

					err = row_merge_insert_index_tuples(
						index[i], old_table,
						OS_FILE_CLOSED, NULL, buf,
						clust_btr_bulk,
						table_total_rows,
						curr_progress,
						pct_cost,
						crypt_block,
						new_table->space_id);

					if (row == NULL) {
						err = clust_btr_bulk->finish(
							err);
						UT_DELETE(clust_btr_bulk);
						clust_btr_bulk = NULL;
					} else {
						/* Release latches for possible
						log_free_chck in spatial index
						build. */
						clust_btr_bulk->release();
					}

					if (err != DB_SUCCESS) {
						break;
					}

					if (row != NULL) {
						/* Restore the cursor on the
						previous clustered index record,
						and empty the buffer. The next
						iteration of the outer loop will
						advance the cursor and read the
						next record (the one which we
						had to ignore due to the buffer
						overflow). */
						mtr.start();
						mtr_started = true;
						if (pcur.restore_position(
							BTR_SEARCH_LEAF, &mtr)
						    == btr_pcur_t::CORRUPTED) {
							goto corrupted_index;
						}
						buf = row_merge_buf_empty(buf);
						merge_buf[i] = buf;
						/* Restart the outer loop on the
						record. We did not insert it
						into any index yet. */
						ut_ad(i == 0);
						break;
					}
				} else if (dict_index_is_unique(buf->index)) {
					row_merge_dup_t	dup = {
						buf->index, table, col_map, 0};

					row_merge_buf_sort(buf, &dup);

					if (dup.n_dup) {
						err = DB_DUPLICATE_KEY;
						trx->error_key_num
							= key_numbers[i];
						break;
					}
				} else {
					row_merge_buf_sort(buf, NULL);
				}
			} else if (online && new_table == old_table) {
				/* Note the newest transaction that
				modified this index when the scan was
				completed. We prevent older readers
				from accessing this index, to ensure
				read consistency. */

				ut_a(row == NULL);

				dict_index_t* index = buf->index;
				index->lock.x_lock(SRW_LOCK_CALL);
				ut_a(dict_index_get_online_status(index)
				     == ONLINE_INDEX_CREATION);

				trx_id_t max_trx_id = row_log_get_max_trx(
					index);

				if (max_trx_id > index->trx_id) {
					index->trx_id = max_trx_id;
				}

				index->lock.x_unlock();
			}

			/* Secondary index and clustered index which is
			not in sorted order can use the temporary file.
			Fulltext index should not use the temporary file. */
			if (!skip_sort && !(buf->index->type & DICT_FTS)) {
				/* In case we can have all rows in sort buffer,
				we can insert directly into the index without
				temporary file if clustered index does not uses
				temporary file. */
				if (row == NULL && file->fd == OS_FILE_CLOSED
				    && !clust_temp_file) {
					DBUG_EXECUTE_IF(
						"row_merge_write_failure",
						err = DB_TEMP_FILE_WRITE_FAIL;
						trx->error_key_num = i;
						goto all_done;);

					DBUG_EXECUTE_IF(
						"row_merge_tmpfile_fail",
						err = DB_OUT_OF_MEMORY;
						trx->error_key_num = i;
						goto all_done;);

					BtrBulk	btr_bulk(index[i], trx);

					err = row_merge_insert_index_tuples(
						index[i], old_table,
						OS_FILE_CLOSED, NULL, buf,
						&btr_bulk,
						table_total_rows,
						curr_progress,
						pct_cost,
						crypt_block,
						new_table->space_id);

					err = btr_bulk.finish(err);

					DBUG_EXECUTE_IF(
						"row_merge_insert_big_row",
						err = DB_TOO_BIG_RECORD;);

					if (err != DB_SUCCESS) {
						break;
					}
				} else {
					if (!row_merge_file_create_if_needed(
						file, tmpfd,
						buf->n_tuples, path)) {
						err = DB_OUT_OF_MEMORY;
						trx->error_key_num = i;
						break;
					}

					/* Ensure that duplicates in the
					clustered index will be detected before
					inserting secondary index records. */
					if (dict_index_is_clust(buf->index)) {
						clust_temp_file = true;
					}

					ut_ad(file->n_rec > 0);

					row_merge_buf_write(buf,
#ifndef DBUG_OFF
							    file,
#endif
							    block);

					if (!row_merge_write(
						    file->fd, file->offset++,
						    block, crypt_block,
						    new_table->space_id)) {
						err = DB_TEMP_FILE_WRITE_FAIL;
						trx->error_key_num = i;
						break;
					}

					MEM_UNDEFINED(
						&block[0], srv_sort_buf_size);
				}
			}
			merge_buf[i] = row_merge_buf_empty(buf);
			buf = merge_buf[i];

			if (UNIV_LIKELY(row != NULL)) {
				/* Try writing the record again, now
				that the buffer has been written out
				and emptied. */

				if (UNIV_UNLIKELY
				    (!(rows_added = row_merge_buf_add(
						buf, fts_index, old_table,
						new_table, psort_info,
						row, ext, &doc_id,
						conv_heap, &err, &v_heap,
						eval_table, trx, col_collate)))) {
                                        /* An empty buffer should have enough
                                        room for at least one record. */
					ut_ad(err == DB_COMPUTE_VALUE_FAILED
					      || err == DB_OUT_OF_MEMORY
					      || err == DB_TOO_BIG_RECORD);
				} else if (err == DB_SUCCESS) {
					file->n_rec += rows_added;
					continue;
				}

				trx->error_key_num = i;
				break;
			}
		}

		if (row == NULL) {
			if (old_table != new_table) {
				new_table->stat_n_rows = n_rows;
			}

			goto all_done;
		}

		if (err != DB_SUCCESS) {
			goto func_exit;
		}

		if (v_heap) {
			mem_heap_empty(v_heap);
		}

		/* Increment innodb_onlineddl_pct_progress status variable */
		read_rows++;
		if(read_rows % 1000 == 0) {
			/* Update progress for each 1000 rows */
			curr_progress = (read_rows >= table_total_rows) ?
					pct_cost :
				pct_cost * static_cast<double>(read_rows)
				/ static_cast<double>(table_total_rows);
			/* presenting 10.12% as 1012 integer */
			onlineddl_pct_progress = (ulint) (curr_progress * 100);
		}
	}

func_exit:
	ut_ad(mtr_started == mtr.is_active());
	if (mtr_started) {
		mtr.commit();
	}
	if (row_heap) {
		mem_heap_free(row_heap);
	}
	ut_free(nonnull);

all_done:
	if (clust_btr_bulk != NULL) {
		ut_ad(err != DB_SUCCESS);
		clust_btr_bulk->latch();
		err = clust_btr_bulk->finish(
			err);
		UT_DELETE(clust_btr_bulk);
	}

	if (prev_fields) {
		ut_free(prev_fields);
		mem_heap_free(mtuple_heap);
	}

	if (v_heap) {
		mem_heap_free(v_heap);
	}

	if (conv_heap != NULL) {
		mem_heap_free(conv_heap);
	}

#ifdef FTS_INTERNAL_DIAG_PRINT
	DEBUG_FTS_SORT_PRINT("FTS_SORT: Complete Scan Table\n");
#endif
	if (UNIV_LIKELY_NULL(fts_parallel_sort_cond)) {
wait_again:
                /* Check if error occurs in child thread */
		for (ulint j = 0; j < fts_sort_pll_degree; j++) {
			if (psort_info[j].error != DB_SUCCESS) {
				err = psort_info[j].error;
				trx->error_key_num = j;
				break;
			}
		}

		/* Tell all children that parent has done scanning */
		for (ulint i = 0; i < fts_sort_pll_degree; i++) {
			if (err == DB_SUCCESS) {
				psort_info[i].state = FTS_PARENT_COMPLETE;
			} else {
				psort_info[i].state = FTS_PARENT_EXITING;
			}
		}

		/* Now wait all children to report back to be completed */
		timespec abstime;
		set_timespec(abstime, 1);
		mysql_mutex_lock(&psort_info[0].mutex);
		my_cond_timedwait(fts_parallel_sort_cond,
				  &psort_info[0].mutex.m_mutex, &abstime);
		mysql_mutex_unlock(&psort_info[0].mutex);

		for (ulint i = 0; i < fts_sort_pll_degree; i++) {
			if (!psort_info[i].child_status) {
				goto wait_again;
			}
		}

		for (ulint j = 0; j < fts_sort_pll_degree; j++) {
			psort_info[j].task->wait();
			delete psort_info[j].task;
		}
	}

#ifdef FTS_INTERNAL_DIAG_PRINT
	DEBUG_FTS_SORT_PRINT("FTS_SORT: Complete Tokenization\n");
#endif
	for (ulint i = 0; i < n_index; i++) {
		row_merge_buf_free(merge_buf[i]);
	}

	row_fts_free_pll_merge_buf(psort_info);

	ut_free(merge_buf);
	ut_free(pcur.old_rec_buf);

	if (sp_tuples != NULL) {
		for (ulint i = 0; i < num_spatial; i++) {
			UT_DELETE(sp_tuples[i]);
		}
		ut_free(sp_tuples);
	}

	/* Update the next Doc ID we used. Table should be locked, so
	no concurrent DML */
	if (max_doc_id && err == DB_SUCCESS) {
		/* Sync fts cache for other fts indexes to keep all
		fts indexes consistent in sync_doc_id. */
		err = fts_sync_table(const_cast<dict_table_t*>(new_table));

		if (err == DB_SUCCESS) {
			new_table->fts->cache->synced_doc_id = max_doc_id;

			/* Update the max value as next FTS_DOC_ID */
			if (max_doc_id >= new_table->fts->cache->next_doc_id) {
				new_table->fts->cache->next_doc_id =
					max_doc_id + 1;
			}

			new_table->fts->cache->first_doc_id =
				new_table->fts->cache->next_doc_id;

			err= fts_update_sync_doc_id(
				new_table,
				new_table->fts->cache->synced_doc_id,
				NULL);
		}
	}

	if (vers_update_trt) {
		trx->mod_tables.emplace(new_table, 0)
			.first->second.set_versioned(0);
	}

	trx->op_info = "";

	DBUG_RETURN(err);
}

/** Write a record via buffer 2 and read the next record to buffer N.
@param N number of the buffer (0 or 1)
@param INDEX record descriptor
@param AT_END statement to execute at end of input */
#define ROW_MERGE_WRITE_GET_NEXT_LOW(N, INDEX, AT_END)			\
	do {								\
		b2 = row_merge_write_rec(&block[2 * srv_sort_buf_size], \
					 &buf[2], b2,			\
					 of->fd, &of->offset,		\
					 mrec##N, offsets##N,		\
			crypt_block ? &crypt_block[2 * srv_sort_buf_size] : NULL , \
					space);				\
		if (UNIV_UNLIKELY(!b2 || ++of->n_rec > file->n_rec)) {	\
			goto corrupt;					\
		}							\
		b##N = row_merge_read_rec(&block[N * srv_sort_buf_size],\
					  &buf[N], b##N, INDEX,		\
					  file->fd, foffs##N,		\
					  &mrec##N, offsets##N,		\
			crypt_block ? &crypt_block[N * srv_sort_buf_size] : NULL, \
					  space);			\
									\
		if (UNIV_UNLIKELY(!b##N)) {				\
			if (mrec##N) {					\
				goto corrupt;				\
			}						\
			AT_END;						\
		}							\
	} while (0)

#ifdef HAVE_PSI_STAGE_INTERFACE
#define ROW_MERGE_WRITE_GET_NEXT(N, INDEX, AT_END)			\
	do {								\
		if (stage != NULL) {					\
			stage->inc();					\
		}							\
		ROW_MERGE_WRITE_GET_NEXT_LOW(N, INDEX, AT_END);		\
	} while (0)
#else /* HAVE_PSI_STAGE_INTERFACE */
#define ROW_MERGE_WRITE_GET_NEXT(N, INDEX, AT_END)			\
	ROW_MERGE_WRITE_GET_NEXT_LOW(N, INDEX, AT_END)
#endif /* HAVE_PSI_STAGE_INTERFACE */

/** Merge two blocks of records on disk and write a bigger block.
@param[in]	dup	descriptor of index being created
@param[in]	file	file containing index entries
@param[in,out]	block	3 buffers
@param[in,out]	foffs0	offset of first source list in the file
@param[in,out]	foffs1	offset of second source list in the file
@param[in,out]	of	output file
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. If not NULL stage->inc() will be called for each record
processed.
@param[in,out]	crypt_block	encryption buffer
@param[in]	space	tablespace ID for encryption
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_merge_blocks(
	const row_merge_dup_t*	dup,
	const merge_file_t*	file,
	row_merge_block_t*	block,
	ulint*			foffs0,
	ulint*			foffs1,
	merge_file_t*		of,
	ut_stage_alter_t*	stage MY_ATTRIBUTE((unused)),
	row_merge_block_t*	crypt_block,
	ulint			space)
{
	mem_heap_t*	heap;	/*!< memory heap for offsets0, offsets1 */

	mrec_buf_t*	buf;	/*!< buffer for handling
				split mrec in block[] */
	const byte*	b0;	/*!< pointer to block[0] */
	const byte*	b1;	/*!< pointer to block[srv_sort_buf_size] */
	byte*		b2;	/*!< pointer to block[2 * srv_sort_buf_size] */
	const mrec_t*	mrec0;	/*!< merge rec, points to block[0] or buf[0] */
	const mrec_t*	mrec1;	/*!< merge rec, points to
				block[srv_sort_buf_size] or buf[1] */
	rec_offs*	offsets0;/* offsets of mrec0 */
	rec_offs*	offsets1;/* offsets of mrec1 */

	DBUG_ENTER("row_merge_blocks");
	DBUG_LOG("ib_merge_sort",
		 "fd=" << file->fd << ',' << *foffs0 << '+' << *foffs1
		 << " to fd=" << of->fd << ',' << of->offset);

	heap = row_merge_heap_create(dup->index, &buf, &offsets0, &offsets1);

	/* Write a record and read the next record.  Split the output
	file in two halves, which can be merged on the following pass. */

	if (!row_merge_read(file->fd, *foffs0, &block[0],
			    crypt_block ? &crypt_block[0] : NULL,
			    space) ||
	    !row_merge_read(file->fd, *foffs1, &block[srv_sort_buf_size],
			    crypt_block ? &crypt_block[srv_sort_buf_size] : NULL,
			    space)) {
corrupt:
		mem_heap_free(heap);
		DBUG_RETURN(DB_CORRUPTION);
	}

	b0 = &block[0];
	b1 = &block[srv_sort_buf_size];
	b2 = &block[2 * srv_sort_buf_size];

	b0 = row_merge_read_rec(
		&block[0], &buf[0], b0, dup->index,
		file->fd, foffs0, &mrec0, offsets0,
		crypt_block ? &crypt_block[0] : NULL,
		space);

	b1 = row_merge_read_rec(
		&block[srv_sort_buf_size],
		&buf[srv_sort_buf_size], b1, dup->index,
		file->fd, foffs1, &mrec1, offsets1,
		crypt_block ? &crypt_block[srv_sort_buf_size] : NULL,
		space);

	if (UNIV_UNLIKELY(!b0 && mrec0)
	    || UNIV_UNLIKELY(!b1 && mrec1)) {

		goto corrupt;
	}

	while (mrec0 && mrec1) {
		int cmp = cmp_rec_rec_simple(
			mrec0, mrec1, offsets0, offsets1,
			dup->index, dup->table);
		if (cmp < 0) {
			ROW_MERGE_WRITE_GET_NEXT(0, dup->index, goto merged);
		} else if (cmp) {
			ROW_MERGE_WRITE_GET_NEXT(1, dup->index, goto merged);
		} else {
			mem_heap_free(heap);
			DBUG_RETURN(DB_DUPLICATE_KEY);
		}
	}

merged:
	if (mrec0) {
		/* append all mrec0 to output */
		for (;;) {
			ROW_MERGE_WRITE_GET_NEXT(0, dup->index, goto done0);
		}
	}
done0:
	if (mrec1) {
		/* append all mrec1 to output */
		for (;;) {
			ROW_MERGE_WRITE_GET_NEXT(1, dup->index, goto done1);
		}
	}
done1:

	mem_heap_free(heap);

	b2 = row_merge_write_eof(
		&block[2 * srv_sort_buf_size],
		b2, of->fd, &of->offset,
		crypt_block ? &crypt_block[2 * srv_sort_buf_size] : NULL,
		space);
	DBUG_RETURN(b2 ? DB_SUCCESS : DB_CORRUPTION);
}

/** Copy a block of index entries.
@param[in]	index	index being created
@param[in]	file	input file
@param[in,out]	block	3 buffers
@param[in,out]	foffs0	input file offset
@param[in,out]	of	output file
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. If not NULL stage->inc() will be called for each record
processed.
@param[in,out]	crypt_block	encryption buffer
@param[in]	space	tablespace ID for encryption
@return TRUE on success, FALSE on failure */
static MY_ATTRIBUTE((warn_unused_result))
ibool
row_merge_blocks_copy(
	const dict_index_t*	index,
	const merge_file_t*	file,
	row_merge_block_t*	block,
	ulint*			foffs0,
	merge_file_t*		of,
	ut_stage_alter_t*	stage MY_ATTRIBUTE((unused)),
	row_merge_block_t*	crypt_block,
	ulint			space)
{
	mem_heap_t*	heap;	/*!< memory heap for offsets0, offsets1 */

	mrec_buf_t*	buf;	/*!< buffer for handling
				split mrec in block[] */
	const byte*	b0;	/*!< pointer to block[0] */
	byte*		b2;	/*!< pointer to block[2 * srv_sort_buf_size] */
	const mrec_t*	mrec0;	/*!< merge rec, points to block[0] */
	rec_offs*	offsets0;/* offsets of mrec0 */
	rec_offs*	offsets1;/* dummy offsets */

	DBUG_ENTER("row_merge_blocks_copy");
	DBUG_LOG("ib_merge_sort",
		 "fd=" << file->fd << ',' << foffs0
		 << " to fd=" << of->fd << ',' << of->offset);

	heap = row_merge_heap_create(index, &buf, &offsets0, &offsets1);

	/* Write a record and read the next record.  Split the output
	file in two halves, which can be merged on the following pass. */

	if (!row_merge_read(file->fd, *foffs0, &block[0],
			crypt_block ? &crypt_block[0] : NULL,
			space)) {
corrupt:
		mem_heap_free(heap);
		DBUG_RETURN(FALSE);
	}

	b0 = &block[0];

	b2 = &block[2 * srv_sort_buf_size];

	b0 = row_merge_read_rec(&block[0], &buf[0], b0, index,
				file->fd, foffs0, &mrec0, offsets0,
				crypt_block ? &crypt_block[0] : NULL,
				space);

	if (UNIV_UNLIKELY(!b0 && mrec0)) {

		goto corrupt;
	}

	if (mrec0) {
		/* append all mrec0 to output */
		for (;;) {
			ROW_MERGE_WRITE_GET_NEXT(0, index, goto done0);
		}
	}
done0:

	/* The file offset points to the beginning of the last page
	that has been read.  Update it to point to the next block. */
	(*foffs0)++;

	mem_heap_free(heap);

	DBUG_RETURN(row_merge_write_eof(
			    &block[2 * srv_sort_buf_size],
			    b2, of->fd, &of->offset,
			    crypt_block
			    ? &crypt_block[2 * srv_sort_buf_size]
			    : NULL, space)
		    != NULL);
}

/** Merge disk files.
@param[in]	trx		transaction
@param[in]	dup		descriptor of index being created
@param[in,out]	file		file containing index entries
@param[in,out]	block		3 buffers
@param[in,out]	tmpfd		temporary file handle
@param[in,out]	num_run		Number of runs that remain to be merged
@param[in,out]	run_offset	Array that contains the first offset number
for each merge run
@param[in,out]	stage		performance schema accounting object, used by
@param[in,out]	crypt_block	encryption buffer
@param[in]	space		tablespace ID for encryption
ALTER TABLE. If not NULL stage->inc() will be called for each record
processed.
@return DB_SUCCESS or error code */
static
dberr_t
row_merge(
	trx_t*			trx,
	const row_merge_dup_t*	dup,
	merge_file_t*		file,
	row_merge_block_t*	block,
	pfs_os_file_t*		tmpfd,
	ulint*			num_run,
	ulint*			run_offset,
	ut_stage_alter_t*	stage,
	row_merge_block_t*	crypt_block,
	ulint			space)
{
	ulint		foffs0;	/*!< first input offset */
	ulint		foffs1;	/*!< second input offset */
	dberr_t		error;	/*!< error code */
	merge_file_t	of;	/*!< output file */
	const ulint	ihalf	= run_offset[*num_run / 2];
				/*!< half the input file */
	ulint		n_run	= 0;
				/*!< num of runs generated from this merge */

	MEM_CHECK_ADDRESSABLE(&block[0], 3 * srv_sort_buf_size);

	if (crypt_block) {
		MEM_CHECK_ADDRESSABLE(&crypt_block[0], 3 * srv_sort_buf_size);
	}

	ut_ad(ihalf < file->offset);

	of.fd = *tmpfd;
	of.offset = 0;
	of.n_rec = 0;

#ifdef POSIX_FADV_SEQUENTIAL
	/* The input file will be read sequentially, starting from the
	beginning and the middle.  In Linux, the POSIX_FADV_SEQUENTIAL
	affects the entire file.  Each block will be read exactly once. */
	posix_fadvise(file->fd, 0, 0,
		      POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);
#endif /* POSIX_FADV_SEQUENTIAL */

	/* Merge blocks to the output file. */
	foffs0 = 0;
	foffs1 = ihalf;

	MEM_UNDEFINED(run_offset, *num_run * sizeof *run_offset);

	for (; foffs0 < ihalf && foffs1 < file->offset; foffs0++, foffs1++) {

		if (trx_is_interrupted(trx)) {
			return(DB_INTERRUPTED);
		}

		/* Remember the offset number for this run */
		run_offset[n_run++] = of.offset;

		error = row_merge_blocks(dup, file, block,
					 &foffs0, &foffs1, &of, stage,
					 crypt_block, space);

		if (error != DB_SUCCESS) {
			return(error);
		}

	}

	/* Copy the last blocks, if there are any. */

	while (foffs0 < ihalf) {

		if (UNIV_UNLIKELY(trx_is_interrupted(trx))) {
			return(DB_INTERRUPTED);
		}

		/* Remember the offset number for this run */
		run_offset[n_run++] = of.offset;

		if (!row_merge_blocks_copy(dup->index, file, block,
					   &foffs0, &of, stage,
					   crypt_block, space)) {
			return(DB_CORRUPTION);
		}
	}

	ut_ad(foffs0 == ihalf);

	while (foffs1 < file->offset) {

		if (trx_is_interrupted(trx)) {
			return(DB_INTERRUPTED);
		}

		/* Remember the offset number for this run */
		run_offset[n_run++] = of.offset;

		if (!row_merge_blocks_copy(dup->index, file, block,
					   &foffs1, &of, stage,
					   crypt_block, space)) {
			return(DB_CORRUPTION);
		}
	}

	ut_ad(foffs1 == file->offset);

	if (UNIV_UNLIKELY(of.n_rec != file->n_rec)) {
		return(DB_CORRUPTION);
	}

	ut_ad(n_run <= *num_run);

	*num_run = n_run;

	/* Each run can contain one or more offsets. As merge goes on,
	the number of runs (to merge) will reduce until we have one
	single run. So the number of runs will always be smaller than
	the number of offsets in file */
	ut_ad((*num_run) <= file->offset);

	/* The number of offsets in output file is always equal or
	smaller than input file */
	ut_ad(of.offset <= file->offset);

	/* Swap file descriptors for the next pass. */
	*tmpfd = file->fd;
	*file = of;

	MEM_UNDEFINED(&block[0], 3 * srv_sort_buf_size);

	return(DB_SUCCESS);
}

/** Merge disk files.
@param[in]	trx	transaction
@param[in]	dup	descriptor of index being created
@param[in,out]	file	file containing index entries
@param[in,out]	block	3 buffers
@param[in,out]	tmpfd	temporary file handle
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. If not NULL, stage->begin_phase_sort() will be called initially
and then stage->inc() will be called for each record processed.
@return DB_SUCCESS or error code */
dberr_t
row_merge_sort(
	trx_t*			trx,
	const row_merge_dup_t*	dup,
	merge_file_t*		file,
	row_merge_block_t*	block,
	pfs_os_file_t*			tmpfd,
	const bool		update_progress,
					/*!< in: update progress
					status variable or not */
	const double 		pct_progress,
					/*!< in: total progress percent
					until now */
	const double		pct_cost, /*!< in: current progress percent */
	row_merge_block_t*	crypt_block, /*!< in: crypt buf or NULL */
	ulint			space,	   /*!< in: space id */
	ut_stage_alter_t* 	stage)
{
	const ulint	half	= file->offset / 2;
	ulint		num_runs;
	ulint*		run_offset;
	dberr_t		error	= DB_SUCCESS;
	ulint		merge_count = 0;
	ulint		total_merge_sort_count;
	double		curr_progress = 0;

	DBUG_ENTER("row_merge_sort");

	/* Record the number of merge runs we need to perform */
	num_runs = file->offset;

	if (stage != NULL) {
		stage->begin_phase_sort(log2(double(num_runs)));
	}

	/* If num_runs are less than 1, nothing to merge */
	if (num_runs <= 1) {
		DBUG_RETURN(error);
	}

	total_merge_sort_count = ulint(ceil(log2(double(num_runs))));

	/* "run_offset" records each run's first offset number */
	run_offset = (ulint*) ut_malloc_nokey(file->offset * sizeof(ulint));

	/* This tells row_merge() where to start for the first round
	of merge. */
	run_offset[half] = half;

	/* The file should always contain at least one byte (the end
	of file marker).  Thus, it must be at least one block. */
	ut_ad(file->offset > 0);

	if (global_system_variables.log_warnings > 2) {
		sql_print_information("InnoDB: Online DDL : merge-sorting"
				      " has estimated " ULINTPF " runs",
				      num_runs);
	}

	/* Merge the runs until we have one big run */
	do {
		error = row_merge(trx, dup, file, block, tmpfd,
				  &num_runs, run_offset, stage,
				  crypt_block, space);

		if(update_progress) {
			merge_count++;
			curr_progress = (merge_count >= total_merge_sort_count) ?
				pct_cost :
				pct_cost * static_cast<double>(merge_count)
				/ static_cast<double>(total_merge_sort_count);
			/* presenting 10.12% as 1012 integer */;
			onlineddl_pct_progress = (ulint) ((pct_progress + curr_progress) * 100);
		}

		if (error != DB_SUCCESS) {
			break;
		}

		MEM_CHECK_DEFINED(run_offset, num_runs * sizeof *run_offset);
	} while (num_runs > 1);

	ut_free(run_offset);

	DBUG_RETURN(error);
}

/** Copy the blob from the given blob file and store it
in field data for the tuple
@param tuple     tuple to be inserted
@param heap      heap to allocate the memory for the blob storage
@param blob_file file to handle blob data */
static dberr_t row_merge_copy_blob_from_file(dtuple_t *tuple,
                                             dict_index_t *index,
                                             mem_heap_t *heap,
                                             merge_file_t *blob_file)
{
  ut_ad(tuple->n_fields == index->n_fields);
  const uint blob_prefix= dict_table_has_atomic_blobs(index->table)
    ? 0
    : REC_ANTELOPE_MAX_INDEX_COL_LEN;
  for (ulint i = index->first_user_field(); i < tuple->n_fields; i++)
  {
    dfield_t *field= dtuple_get_nth_field(tuple, i);
    const byte *field_data= static_cast<byte*>(dfield_get_data(field));
    ulint field_len= dfield_get_len(field);
    if (!dfield_is_ext(field))
      continue;

    ut_a(field_len >= BTR_EXTERN_FIELD_REF_SIZE);
    ut_ad(!dfield_is_null(field));

    field_data += blob_prefix;
    ut_ad(mach_read_from_8(field_data) == 0);
    uint64_t offset= mach_read_from_8(field_data + 8);
    uint32_t len= mach_read_from_4(field_data + 16);

    byte *data= (byte*) mem_heap_alloc(heap, blob_prefix + len);
    memcpy(data, field->data, blob_prefix);
    if (dberr_t err= os_file_read(IORequestRead, blob_file->fd,
                                  data + blob_prefix,
                                  offset, len, nullptr))
      return err;
    dfield_set_data(field, data, blob_prefix + len);
  }

  return DB_SUCCESS;
}

/** Copy externally stored columns to the data tuple.
@param[in]	mrec		record containing BLOB pointers,
or NULL to use tuple instead
@param[in]	offsets		offsets of mrec
@param[in]	zip_size	compressed page size in bytes, or 0
@param[in,out]	tuple		data tuple
@param[in,out]	heap		memory heap */
static
void
row_merge_copy_blobs(
	const mrec_t*		mrec,
	const rec_offs*		offsets,
	ulint			zip_size,
	dtuple_t*		tuple,
	mem_heap_t*		heap)
{
	ut_ad(mrec == NULL || rec_offs_any_extern(offsets));

	for (ulint i = 0; i < dtuple_get_n_fields(tuple); i++) {
		ulint		len;
		const void*	data;
		dfield_t*	field = dtuple_get_nth_field(tuple, i);
		ulint		field_len;
		const byte*	field_data;

		if (!dfield_is_ext(field)) {
			continue;
		}

		ut_ad(!dfield_is_null(field));

		/* During the creation of a PRIMARY KEY, the table is
		X-locked, and we skip copying records that have been
		marked for deletion. Therefore, externally stored
		columns cannot possibly be freed between the time the
		BLOB pointers are read (row_merge_read_clustered_index())
		and dereferenced (below). */
		if (mrec == NULL) {
			field_data
				= static_cast<byte*>(dfield_get_data(field));
			field_len = dfield_get_len(field);

			ut_a(field_len >= BTR_EXTERN_FIELD_REF_SIZE);

			ut_a(memcmp(field_data + field_len
				     - BTR_EXTERN_FIELD_REF_SIZE,
				     field_ref_zero,
				     BTR_EXTERN_FIELD_REF_SIZE));

			data = btr_copy_externally_stored_field(
				&len, field_data, zip_size, field_len, heap);
		} else {
			data = btr_rec_copy_externally_stored_field(
				mrec, offsets, zip_size, i, &len, heap);
		}

		/* Because we have locked the table, any records
		written by incomplete transactions must have been
		rolled back already. There must not be any incomplete
		BLOB columns. */
		ut_a(data);

		dfield_set_data(field, data, len);
	}
}

/** Convert a merge record to a typed data tuple. Note that externally
stored fields are not copied to heap.
@param[in,out]	index	index on the table
@param[in]	mtuple	merge record
@param[in]	heap	memory heap from which memory needed is allocated
@return	index entry built. */
static
void
row_merge_mtuple_to_dtuple(
	dict_index_t*	index,
	dtuple_t*	dtuple,
	const mtuple_t* mtuple)
{
	memcpy(dtuple->fields, mtuple->fields,
	       dtuple->n_fields * sizeof *mtuple->fields);
}

static	MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_merge_insert_index_tuples(
	dict_index_t*		index,
	const dict_table_t*	old_table,
	const pfs_os_file_t&	fd,
	row_merge_block_t*	block,
	const row_merge_buf_t*	row_buf,
	BtrBulk*		btr_bulk,
	const ib_uint64_t	table_total_rows,
	double			pct_progress,
	double			pct_cost,
	row_merge_block_t*	crypt_block,
	ulint			space,
	ut_stage_alter_t*	stage,
	merge_file_t*		blob_file)
{
	const byte*		b;
	mem_heap_t*		heap;
	mem_heap_t*		tuple_heap;
	dberr_t			error = DB_SUCCESS;
	ulint			foffs = 0;
	rec_offs*		offsets;
	mrec_buf_t*		buf;
	ulint			n_rows = 0;
	dtuple_t*		dtuple;
	ib_uint64_t		inserted_rows = 0;
	double			curr_progress = 0;
	dict_index_t*		old_index = NULL;
	const mrec_t*		mrec  = NULL;
	mtr_t			mtr;


	DBUG_ENTER("row_merge_insert_index_tuples");

	ut_ad(!srv_read_only_mode);
	ut_ad(!(index->type & DICT_FTS));
	ut_ad(!dict_index_is_spatial(index));

	if (stage != NULL) {
		stage->begin_phase_insert();
	}

	tuple_heap = mem_heap_create(1000);

	{
		ulint i	= 1 + REC_OFFS_HEADER_SIZE
			+ dict_index_get_n_fields(index);
		heap = mem_heap_create(sizeof *buf + i * sizeof *offsets);
		offsets = static_cast<rec_offs*>(
			mem_heap_alloc(heap, i * sizeof *offsets));
		rec_offs_set_n_alloc(offsets, i);
		rec_offs_set_n_fields(offsets, dict_index_get_n_fields(index));
	}

	if (row_buf != NULL) {
		ut_ad(fd == OS_FILE_CLOSED);
		ut_ad(block == NULL);
		DBUG_EXECUTE_IF("row_merge_read_failure",
				error = DB_CORRUPTION;
				goto err_exit;);
		buf = NULL;
		b = NULL;
		dtuple = dtuple_create(
			heap, dict_index_get_n_fields(index));
		dtuple_set_n_fields_cmp(
			dtuple, dict_index_get_n_unique_in_tree(index));
	} else {
		b = block;
		dtuple = NULL;

		if (!row_merge_read(fd, foffs, block, crypt_block, space)) {
			error = DB_CORRUPTION;
			goto err_exit;
		} else {
			buf = static_cast<mrec_buf_t*>(
				mem_heap_alloc(heap, sizeof *buf));
		}
	}

	for (;;) {

		if (stage != NULL) {
			stage->inc();
		}

		if (row_buf != NULL) {
			if (n_rows >= row_buf->n_tuples) {
				break;
			}

			/* Convert merge tuple record from
			row buffer to data tuple record */
			row_merge_mtuple_to_dtuple(
				index, dtuple, &row_buf->tuples[n_rows]);
			n_rows++;
			/* BLOB pointers must be copied from dtuple */
			mrec = NULL;
		} else {
			b = row_merge_read_rec(block, buf, b, index,
					       fd, &foffs, &mrec, offsets,
					       crypt_block,
					       space);

			if (UNIV_UNLIKELY(!b)) {
				/* End of list, or I/O error */
				if (mrec) {
					error = DB_CORRUPTION;
				}
				break;
			}

			dtuple = row_rec_to_index_entry_low(
				mrec, index, offsets, tuple_heap);
		}

		old_index	= dict_table_get_first_index(old_table);

		if (dict_index_is_clust(index)
		    && dict_index_is_online_ddl(old_index)) {
			error = row_log_table_get_error(old_index);
			if (error != DB_SUCCESS) {
				break;
			}
		}

		ut_ad(!dtuple_get_n_ext(dtuple) || index->is_primary());

		if (!dtuple_get_n_ext(dtuple)) {
		} else if (blob_file) {
			error = row_merge_copy_blob_from_file(
				dtuple, index, tuple_heap, blob_file);
			if (error != DB_SUCCESS) {
				break;
			}
		} else {
			/* Off-page columns can be fetched safely
			when concurrent modifications to the table
			are disabled. (Purge can process delete-marked
			records, but row_merge_read_clustered_index()
			would have skipped them.)

			When concurrent modifications are enabled,
			row_merge_read_clustered_index() will
			only see rows from transactions that were
			committed before the ALTER TABLE started
			(REPEATABLE READ).

			Any modifications after the
			row_merge_read_clustered_index() scan
			will go through row_log_table_apply(). */
			row_merge_copy_blobs(
				mrec, offsets,
				old_table->space->zip_size(),
				dtuple, tuple_heap);
		}

		ut_ad(dtuple_validate(dtuple));
		error = btr_bulk->insert(dtuple);

		if (error != DB_SUCCESS) {
			goto err_exit;
		}

		mem_heap_empty(tuple_heap);

		/* Increment innodb_onlineddl_pct_progress status variable */
		inserted_rows++;
		if(inserted_rows % 1000 == 0) {
			/* Update progress for each 1000 rows */
			curr_progress = (inserted_rows >= table_total_rows ||
				table_total_rows <= 0) ?
				pct_cost :
				pct_cost * static_cast<double>(inserted_rows)
				/ static_cast<double>(table_total_rows);

			/* presenting 10.12% as 1012 integer */;
			onlineddl_pct_progress = (ulint) ((pct_progress + curr_progress) * 100);
		}
	}

err_exit:
	mem_heap_free(tuple_heap);
	mem_heap_free(heap);

	DBUG_RETURN(error);
}

/*********************************************************************//**
Drop an index that was created before an error occurred.
The data dictionary must have been locked exclusively by the caller,
because the transaction will not be committed. */
static
void
row_merge_drop_index_dict(
/*======================*/
	trx_t*		trx,	/*!< in/out: dictionary transaction */
	index_id_t	index_id)/*!< in: index identifier */
{
	static const char sql[] =
		"PROCEDURE DROP_INDEX_PROC () IS\n"
		"BEGIN\n"
		"DELETE FROM SYS_FIELDS WHERE INDEX_ID=:indexid;\n"
		"DELETE FROM SYS_INDEXES WHERE ID=:indexid;\n"
		"END;\n";
	dberr_t		error;
	pars_info_t*	info;

	ut_ad(!srv_read_only_mode);
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(trx->dict_operation);
	ut_ad(dict_sys.locked());

	info = pars_info_create();
	pars_info_add_ull_literal(info, "indexid", index_id);
	trx->op_info = "dropping index from dictionary";
	error = que_eval_sql(info, sql, trx);

	if (error != DB_SUCCESS) {
		/* Even though we ensure that DDL transactions are WAIT
		and DEADLOCK free, we could encounter other errors e.g.,
		DB_TOO_MANY_CONCURRENT_TRXS. */
		trx->error_state = DB_SUCCESS;

		ib::error() << "row_merge_drop_index_dict failed with error "
			<< error;
	}

	trx->op_info = "";
}

/*********************************************************************//**
Drop indexes that were created before an error occurred.
The data dictionary must have been locked exclusively by the caller,
because the transaction will not be committed. */
static
void
row_merge_drop_indexes_dict(
/*========================*/
	trx_t*		trx,	/*!< in/out: dictionary transaction */
	table_id_t	table_id)/*!< in: table identifier */
{
	static const char sql[] =
		"PROCEDURE DROP_INDEXES_PROC () IS\n"
		"ixid CHAR;\n"
		"found INT;\n"

		"DECLARE CURSOR index_cur IS\n"
		" SELECT ID FROM SYS_INDEXES\n"
		" WHERE TABLE_ID=:tableid AND\n"
		" SUBSTR(NAME,0,1)='" TEMP_INDEX_PREFIX_STR "'\n"
		"FOR UPDATE;\n"

		"BEGIN\n"
		"found := 1;\n"
		"OPEN index_cur;\n"
		"WHILE found = 1 LOOP\n"
		"  FETCH index_cur INTO ixid;\n"
		"  IF (SQL % NOTFOUND) THEN\n"
		"    found := 0;\n"
		"  ELSE\n"
		"    DELETE FROM SYS_FIELDS WHERE INDEX_ID=ixid;\n"
		"    DELETE FROM SYS_INDEXES WHERE CURRENT OF index_cur;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE index_cur;\n"

		"END;\n";
	dberr_t		error;
	pars_info_t*	info;

	ut_ad(!srv_read_only_mode);
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(trx->dict_operation);
	ut_ad(dict_sys.locked());

	/* It is possible that table->n_ref_count > 1 when
	locked=TRUE. In this case, all code that should have an open
	handle to the table be waiting for the next statement to execute,
	or waiting for a meta-data lock.

	A concurrent purge will be prevented by dict_sys.latch. */

	info = pars_info_create();
	pars_info_add_ull_literal(info, "tableid", table_id);
	trx->op_info = "dropping indexes";
	error = que_eval_sql(info, sql, trx);

	switch (error) {
	case DB_SUCCESS:
		break;
	default:
		/* Even though we ensure that DDL transactions are WAIT
		and DEADLOCK free, we could encounter other errors e.g.,
		DB_TOO_MANY_CONCURRENT_TRXS. */
		ib::error() << "row_merge_drop_indexes_dict failed with error "
			<< error;
		/* fall through */
	case DB_TOO_MANY_CONCURRENT_TRXS:
		trx->error_state = DB_SUCCESS;
	}

	trx->op_info = "";
}

/** Drop common internal tables if all fulltext indexes are dropped
@param trx   transaction
@param table user table */
static void row_merge_drop_fulltext_indexes(trx_t *trx, dict_table_t *table)
{
  if (DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID) ||
      !table->fts ||
      !ib_vector_is_empty(table->fts->indexes))
    return;

  for (const dict_index_t *index= dict_table_get_first_index(table);
       index; index= dict_table_get_next_index(index))
    if (index->type & DICT_FTS)
      return;

  fts_optimize_remove_table(table);
  fts_drop_tables(trx, *table);
  table->fts->~fts_t();
  table->fts= nullptr;
  DICT_TF2_FLAG_UNSET(table, DICT_TF2_FTS);
}

/** Drop indexes that were created before an error occurred.
The data dictionary must have been locked exclusively by the caller,
because the transaction will not be committed.
@param trx              dictionary transaction
@param table            table containing the indexes
@param locked           True if table is locked,
                        false - may need to do lazy drop
@param alter_trx        Alter table transaction */
void
row_merge_drop_indexes(
        trx_t*          trx,
        dict_table_t*   table,
        bool            locked,
        const trx_t*    alter_trx)
{
	dict_index_t*	index;
	dict_index_t*	next_index;

	ut_ad(!srv_read_only_mode);
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(trx->dict_operation);
	ut_ad(dict_sys.locked());

	index = dict_table_get_first_index(table);
	ut_ad(dict_index_is_clust(index));
	ut_ad(dict_index_get_online_status(index) == ONLINE_INDEX_COMPLETE);

	/* the caller should have an open handle to the table */
	ut_ad(table->get_ref_count() >= 1);

	/* It is possible that table->n_ref_count > 1 when
	locked=TRUE. In this case, all code that should have an open
	handle to the table be waiting for the next statement to execute,
	or waiting for a meta-data lock.

	A concurrent purge will be prevented by MDL. */

	if (!locked && (table->get_ref_count() > 1
			|| table->has_lock_other_than(alter_trx))) {
		while ((index = dict_table_get_next_index(index)) != NULL) {
			ut_ad(!dict_index_is_clust(index));

			switch (dict_index_get_online_status(index)) {
			case ONLINE_INDEX_ABORTED_DROPPED:
				continue;
			case ONLINE_INDEX_COMPLETE:
				if (index->is_committed()) {
					/* Do nothing to already
					published indexes. */
				} else if (index->type & DICT_FTS) {
					/* Drop a completed FULLTEXT
					index, due to a timeout during
					MDL upgrade for
					commit_inplace_alter_table().
					Because only concurrent reads
					are allowed (and they are not
					seeing this index yet) we
					are safe to drop the index. */
					dict_index_t* prev = UT_LIST_GET_PREV(
						indexes, index);
					/* At least there should be
					the clustered index before
					this one. */
					ut_ad(prev);
					ut_a(table->fts);
					fts_drop_index(table, index, trx);
					row_merge_drop_index_dict(
						trx, index->id);
					/* We can remove a DICT_FTS
					index from the cache, because
					we do not allow ADD FULLTEXT INDEX
					with LOCK=NONE. If we allowed that,
					we should exclude FTS entries from
					prebuilt->ins_node->entry_list
					in ins_node_create_entry_list(). */
#ifdef BTR_CUR_HASH_ADAPT
					ut_ad(!index->search_info.ref_count);
#endif /* BTR_CUR_HASH_ADAPT */
					dict_index_remove_from_cache(
						table, index);
					index = prev;
				} else {
					index->lock.x_lock(SRW_LOCK_CALL);
					dict_index_set_online_status(
						index, ONLINE_INDEX_ABORTED);
					index->type |= DICT_CORRUPT;
					table->drop_aborted = TRUE;
					goto drop_aborted;
				}
				continue;
			case ONLINE_INDEX_CREATION:
				index->lock.x_lock(SRW_LOCK_CALL);
				ut_ad(!index->is_committed());
				row_log_abort_sec(index);
			drop_aborted:
				index->lock.x_unlock();

				DEBUG_SYNC_C("merge_drop_index_after_abort");
				/* covered by dict_sys.latch */
				MONITOR_INC(MONITOR_BACKGROUND_DROP_INDEX);
				/* fall through */
			case ONLINE_INDEX_ABORTED:
				/* Drop the index tree from the
				data dictionary and free it from
				the tablespace, but keep the object
				in the data dictionary cache. */
				row_merge_drop_index_dict(trx, index->id);
				index->lock.x_lock(SRW_LOCK_CALL);
				dict_index_set_online_status(
					index, ONLINE_INDEX_ABORTED_DROPPED);
				index->lock.x_unlock();
				table->drop_aborted = TRUE;
				continue;
			}
			ut_error;
		}

		row_merge_drop_fulltext_indexes(trx, table);
		return;
	}

	row_merge_drop_indexes_dict(trx, table->id);

	/* Invalidate all row_prebuilt_t::ins_graph that are referring
	to this table. That is, force row_get_prebuilt_insert_row() to
	rebuild prebuilt->ins_node->entry_list). */
	if (table->def_trx_id < trx->id) {
		table->def_trx_id = trx->id;
	} else {
		ut_ad(table->def_trx_id == trx->id || table->name.part());
	}

	next_index = dict_table_get_next_index(index);

	while ((index = next_index) != NULL) {
		/* read the next pointer before freeing the index */
		next_index = dict_table_get_next_index(index);

		ut_ad(!dict_index_is_clust(index));

		if (!index->is_committed()) {
			/* If it is FTS index, drop from table->fts
			and also drop its auxiliary tables */
			if (index->type & DICT_FTS) {
				ut_a(table->fts);
				fts_drop_index(table, index, trx);
			}

			switch (dict_index_get_online_status(index)) {
			case ONLINE_INDEX_CREATION:
				/* This state should only be possible
				when prepare_inplace_alter_table() fails
				after invoking row_merge_create_index().
				In inplace_alter_table(),
				row_merge_build_indexes()
				should never leave the index in this state.
				It would invoke row_log_abort_sec() on
				failure. */
			case ONLINE_INDEX_COMPLETE:
				/* In these cases, we are able to drop
				the index straight. The DROP INDEX was
				never deferred. */
				break;
			case ONLINE_INDEX_ABORTED:
			case ONLINE_INDEX_ABORTED_DROPPED:
				/* covered by dict_sys.latch */
				MONITOR_DEC(MONITOR_BACKGROUND_DROP_INDEX);
			}

			dict_index_remove_from_cache(table, index);
		}
	}

	row_merge_drop_fulltext_indexes(trx, table);
	table->drop_aborted = FALSE;
	ut_d(dict_table_check_for_dup_indexes(table, CHECK_ALL_COMPLETE));
}

/** Drop fulltext indexes */
static ibool row_merge_drop_fts(void *node, void *trx)
{
   auto s= static_cast<sel_node_t*>(node);

   const dfield_t *table_id= que_node_get_val(s->select_list);
   ut_ad(table_id->type.mtype == DATA_BINARY);
   node= que_node_get_next(s->select_list);
   ut_ad(!que_node_get_next(node));
   const dfield_t *index_id= que_node_get_val(node);
   ut_ad(index_id->type.mtype == DATA_BINARY);

   static const char sql[]=
     "PROCEDURE DROP_TABLES_PROC () IS\n"
     "tid CHAR;\n"
     "iid CHAR;\n"

     "DECLARE CURSOR cur_tab IS\n"
     "SELECT ID FROM SYS_TABLES\n"
     "WHERE INSTR(NAME,:name)+45=LENGTH(NAME)"
     " AND INSTR('123456',SUBSTR(NAME,LENGTH(NAME)-1,1))>0"
     " FOR UPDATE;\n"

     "DECLARE CURSOR cur_idx IS\n"
     "SELECT ID FROM SYS_INDEXES\n"
     "WHERE TABLE_ID = tid FOR UPDATE;\n"

     "BEGIN\n"
     "OPEN cur_tab;\n"
     "WHILE 1 = 1 LOOP\n"
     "  FETCH cur_tab INTO tid;\n"
     "  IF (SQL % NOTFOUND) THEN EXIT; END IF;\n"
     "  OPEN cur_idx;\n"
     "  WHILE 1 = 1 LOOP\n"
     "    FETCH cur_idx INTO iid;\n"
     "    IF (SQL % NOTFOUND) THEN EXIT; END IF;\n"
     "    DELETE FROM SYS_FIELDS WHERE INDEX_ID=iid;\n"
     "    DELETE FROM SYS_INDEXES WHERE CURRENT OF cur_idx;\n"
     "  END LOOP;\n"
     "  CLOSE cur_idx;\n"
     "  DELETE FROM SYS_COLUMNS WHERE TABLE_ID=tid;\n"
     "  DELETE FROM SYS_TABLES WHERE CURRENT OF cur_tab;\n"
     "END LOOP;\n"
     "CLOSE cur_tab;\n"
     "END;\n";

   if (table_id->len == 8 && index_id->len == 8)
   {
     char buf[sizeof "/FTS_0000000000000000_0000000000000000_INDEX_"];
     snprintf(buf, sizeof buf, "/FTS_%016llx_%016llx_INDEX_",
              static_cast<ulonglong>
              (mach_read_from_8(static_cast<const byte*>(table_id->data))),
              static_cast<ulonglong>
              (mach_read_from_8(static_cast<const byte*>(index_id->data))));
     auto pinfo= pars_info_create();
     pars_info_add_str_literal(pinfo, "name", buf);
     que_eval_sql(pinfo, sql, static_cast<trx_t*>(trx));
   }

   return true;
}

/** During recovery, drop recovered index stubs that were created in
prepare_inplace_alter_table_dict(). */
void row_merge_drop_temp_indexes()
{
	static_assert(DICT_FTS == 32, "compatibility");

	static const char sql[] =
		"PROCEDURE DROP_TEMP_INDEXES_PROC () IS\n"
		"ixid CHAR;\n"
		"found INT;\n"

		"DECLARE FUNCTION drop_fts;\n"

		"DECLARE CURSOR fts_cur IS\n"
		" SELECT TABLE_ID,ID FROM SYS_INDEXES\n"
		" WHERE TYPE=32"
		" AND SUBSTR(NAME,0,1)='" TEMP_INDEX_PREFIX_STR "'\n"
		" FOR UPDATE;\n"

		"DECLARE CURSOR index_cur IS\n"
		" SELECT ID FROM SYS_INDEXES\n"
		" WHERE SUBSTR(NAME,0,1)='" TEMP_INDEX_PREFIX_STR "'\n"
		"FOR UPDATE;\n"

		"BEGIN\n"
		"found := 1;\n"
		"OPEN fts_cur;\n"
		"WHILE found = 1 LOOP\n"
		"  FETCH fts_cur INTO drop_fts();\n"
		"  IF (SQL % NOTFOUND) THEN\n"
		"    found := 0;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE fts_cur;\n"

		"OPEN index_cur;\n"
		"WHILE found = 1 LOOP\n"
		"  FETCH index_cur INTO ixid;\n"
		"  IF (SQL % NOTFOUND) THEN\n"
		"    found := 0;\n"
		"  ELSE\n"
		"    DELETE FROM SYS_FIELDS WHERE INDEX_ID=ixid;\n"
		"    DELETE FROM SYS_INDEXES WHERE CURRENT OF index_cur;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE index_cur;\n"
		"END;\n";

	/* Load the table definitions that contain partially defined
	indexes, so that the data dictionary information can be checked
	when accessing the tablename.ibd files. */
	trx_t* trx = trx_create();
	trx_start_for_ddl(trx);
	trx->op_info = "dropping partially created indexes";
	dberr_t error = lock_sys_tables(trx);

	row_mysql_lock_data_dictionary(trx);
	/* Ensure that this transaction will be rolled back and locks
	will be released, if the server gets killed before the commit
	gets written to the redo log. */
	trx->dict_operation = true;

	trx->op_info = "dropping indexes";

	pars_info_t* pinfo = pars_info_create();
	pars_info_bind_function(pinfo, "drop_fts", row_merge_drop_fts, trx);
	if (error == DB_SUCCESS) {
		error = que_eval_sql(pinfo, sql, trx);
	}

	if (error) {
		/* Even though we ensure that DDL transactions are WAIT
		and DEADLOCK free, we could encounter other errors e.g.,
		DB_TOO_MANY_CONCURRENT_TRXS. */
		trx->error_state = DB_SUCCESS;

		ib::error() << "row_merge_drop_temp_indexes(): " << error;
	}

	trx_commit_for_mysql(trx);
	row_mysql_unlock_data_dictionary(trx);
	trx->free();
}


/** Create temporary merge files in the given parameter path, and if
UNIV_PFS_IO is defined, register the file descriptor with Performance Schema.
@param[in]	path	location for creating temporary merge files, or NULL
@return File descriptor */
static pfs_os_file_t row_merge_file_create_mode(const char *path, int mode)
{
	if (!path) {
		path = mysql_tmpdir;
	}
#ifdef UNIV_PFS_IO
	/* This temp file open does not go through normal
	file APIs, add instrumentation to register with
	performance schema */
	struct PSI_file_locker*	locker;
	PSI_file_locker_state	state;
	static const char label[] = "/Innodb Merge Temp File";
	char* name = static_cast<char*>(
		ut_malloc_nokey(strlen(path) + sizeof label));
	strcpy(name, path);
	strcat(name, label);

	register_pfs_file_open_begin(
		&state, locker, innodb_temp_file_key,
		PSI_FILE_CREATE, path ? name : label, __FILE__, __LINE__);

#endif
	DBUG_ASSERT(strlen(path) + 2 <= FN_REFLEN);
	char filename[FN_REFLEN];
	File f = create_temp_file(filename, path, "ib",
				  O_BINARY | O_SEQUENTIAL,
				  MYF(MY_WME | MY_TEMPORARY));
	pfs_os_file_t fd = IF_WIN((os_file_t)my_get_osfhandle(f), f);

#ifdef UNIV_PFS_IO
	register_pfs_file_open_end(locker, fd, 
		(fd == OS_FILE_CLOSED)?NULL:&fd);
	ut_free(name);
#endif

	if (fd == OS_FILE_CLOSED) {
		ib::error() << "Cannot create temporary merge file";
	}
	return(fd);
}

/** Create a temporary file at the specified path.
@param path location for creating temporary merge files, or nullptr
@return File descriptor */
pfs_os_file_t row_merge_file_create_low(const char *path)
{
  return row_merge_file_create_mode(path, O_BINARY | O_SEQUENTIAL);
}

/** Create a merge file in the given location.
@param[out]	merge_file	merge file structure
@param[in]	path		location for creating temporary file, or NULL
@return file descriptor, or OS_FILE_CLOSED on error */
pfs_os_file_t
row_merge_file_create(
	merge_file_t*	merge_file,
	const char*	path)
{
	merge_file->offset = 0;
	merge_file->n_rec = 0;
	merge_file->fd =
		row_merge_file_create_mode(path,
#if !defined _WIN32 && defined O_DIRECT
					   srv_disable_sort_file_cache
					   ? O_DIRECT | O_BINARY | O_SEQUENTIAL
					   :
#endif
					   O_BINARY | O_SEQUENTIAL);
	return(merge_file->fd);
}

/*********************************************************************//**
Destroy a merge file. And de-register the file from Performance Schema
if UNIV_PFS_IO is defined. */
void
row_merge_file_destroy_low(
/*=======================*/
	const pfs_os_file_t& fd)	/*!< in: merge file descriptor */
{
	if (fd != OS_FILE_CLOSED) {
		int res = mysql_file_close(IF_WIN(my_win_handle2File((os_file_t)fd), fd),
					   MYF(MY_WME));
		ut_a(res != -1);
	}
}
/*********************************************************************//**
Destroy a merge file. */
void
row_merge_file_destroy(
/*===================*/
	merge_file_t*	merge_file)	/*!< in/out: merge file structure */
{
	ut_ad(!srv_read_only_mode);

	if (merge_file->fd != OS_FILE_CLOSED) {
		row_merge_file_destroy_low(merge_file->fd);
		merge_file->fd = OS_FILE_CLOSED;
	}
}

/*********************************************************************//**
Rename an index in the dictionary that was created. The data
dictionary must have been locked exclusively by the caller, because
the transaction will not be committed.
@return DB_SUCCESS if all OK */
dberr_t
row_merge_rename_index_to_add(
/*==========================*/
	trx_t*		trx,		/*!< in/out: transaction */
	table_id_t	table_id,	/*!< in: table identifier */
	index_id_t	index_id)	/*!< in: index identifier */
{
	dberr_t		err = DB_SUCCESS;
	pars_info_t*	info = pars_info_create();

	/* We use the private SQL parser of Innobase to generate the
	query graphs needed in renaming indexes. */

	static const char rename_index[] =
		"PROCEDURE RENAME_INDEX_PROC () IS\n"
		"BEGIN\n"
		"UPDATE SYS_INDEXES SET NAME=SUBSTR(NAME,1,LENGTH(NAME)-1)\n"
		"WHERE TABLE_ID = :tableid AND ID = :indexid;\n"
		"END;\n";

	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(trx->dict_operation);

	trx->op_info = "renaming index to add";

	pars_info_add_ull_literal(info, "tableid", table_id);
	pars_info_add_ull_literal(info, "indexid", index_id);

	err = que_eval_sql(info, rename_index, trx);

	if (err != DB_SUCCESS) {
		/* Even though we ensure that DDL transactions are WAIT
		and DEADLOCK free, we could encounter other errors e.g.,
		DB_TOO_MANY_CONCURRENT_TRXS. */
		trx->error_state = DB_SUCCESS;

		ib::error() << "row_merge_rename_index_to_add failed with"
			" error " << err;
	}

	trx->op_info = "";

	return(err);
}

/** Create the index and load in to the dictionary.
@param[in,out]	table		the index is on this table
@param[in]	index_def	the index definition
@param[in]	add_v		new virtual columns added along with add
				index call
@return index, or NULL on error */
dict_index_t*
row_merge_create_index(
	dict_table_t*		table,
	const index_def_t*	index_def,
	const dict_add_v_col_t*	add_v)
{
	dict_index_t*	index;
	ulint		n_fields = index_def->n_fields;
	ulint		i;
	ulint		n_add_vcol = 0;

	DBUG_ENTER("row_merge_create_index");

	ut_ad(!srv_read_only_mode);

	/* Create the index prototype, using the passed in def, this is not
	a persistent operation. We pass 0 as the space id, and determine at
	a lower level the space id where to store the table. */

	index = dict_mem_index_create(table, index_def->name,
				      index_def->ind_type, n_fields);
	index->set_committed(index_def->rebuild);

	for (i = 0; i < n_fields; i++) {
		const char*	name;
		index_field_t*	ifield = &index_def->fields[i];

		if (ifield->is_v_col) {
			if (ifield->col_no >= table->n_v_def) {
				ut_ad(ifield->col_no < table->n_v_def
				      + add_v->n_v_col);
				ut_ad(ifield->col_no >= table->n_v_def);
				name = add_v->v_col_name[
					ifield->col_no - table->n_v_def];
				n_add_vcol++;
			} else {
				name = dict_table_get_v_col_name(
					table, ifield->col_no).str;
			}
		} else {
			name = dict_table_get_col_name(table, ifield->col_no).str;
		}

		dict_mem_index_add_field(index, name, ifield->prefix_len,
					 ifield->descending);
	}

	if (n_add_vcol) {
		index->assign_new_v_col(n_add_vcol);
	}

	DBUG_RETURN(index);
}

/*********************************************************************//**
Check if a transaction can use an index. */
bool
row_merge_is_index_usable(
/*======================*/
	const trx_t*		trx,	/*!< in: transaction */
	const dict_index_t*	index)	/*!< in: index to check */
{
	if (!index->is_primary()
	    && dict_index_is_online_ddl(index)) {
		/* Indexes that are being created are not useable. */
		return(false);
	}

	return(!index->is_corrupted()
	       && (index->table->is_temporary() || index->table->no_rollback()
		   || index->trx_id == 0
		   || !trx->read_view.is_open()
		   || trx->read_view.changes_visible(index->trx_id)));
}

/** Build indexes on a table by reading a clustered index, creating a temporary
file containing index entries, merge sorting these index entries and inserting
sorted index entries to indexes.
@param[in]	trx		transaction
@param[in]	old_table	table where rows are read from
@param[in]	new_table	table where indexes are created; identical to
old_table unless creating a PRIMARY KEY
@param[in]	online		true if creating indexes online
@param[in]	indexes		indexes to be created
@param[in]	key_numbers	MySQL key numbers
@param[in]	n_indexes	size of indexes[]
@param[in,out]	table		MySQL table, for reporting erroneous key value
if applicable
@param[in]	defaults	default values of added, changed columns, or NULL
@param[in]	col_map		mapping of old column numbers to new ones, or
NULL if old_table == new_table
@param[in]	add_autoinc	number of added AUTO_INCREMENT columns, or
ULINT_UNDEFINED if none is added
@param[in,out]	sequence	autoinc sequence
@param[in]	skip_pk_sort	whether the new PRIMARY KEY will follow
existing order
@param[in,out]	stage		performance schema accounting object, used by
ALTER TABLE. stage->begin_phase_read_pk() will be called at the beginning of
this function and it will be passed to other functions for further accounting.
@param[in]	add_v		new virtual columns added along with indexes
@param[in]	eval_table	mysql table used to evaluate virtual column
				value, see innobase_get_computed_value().
@param[in]	allow_not_null	allow the conversion from null to not-null
@param[in]	col_collate	columns whose collations changed, or nullptr
@return DB_SUCCESS or error code */
dberr_t
row_merge_build_indexes(
	trx_t*			trx,
	dict_table_t*		old_table,
	dict_table_t*		new_table,
	bool			online,
	dict_index_t**		indexes,
	const ulint*		key_numbers,
	ulint			n_indexes,
	struct TABLE*		table,
	const dtuple_t*		defaults,
	const ulint*		col_map,
	ulint			add_autoinc,
	ib_sequence_t&		sequence,
	bool			skip_pk_sort,
	ut_stage_alter_t*	stage,
	const dict_add_v_col_t*	add_v,
	struct TABLE*		eval_table,
	bool			allow_not_null,
	const col_collations*	col_collate)
{
	merge_file_t*		merge_files;
	row_merge_block_t*	block;
	ut_new_pfx_t		block_pfx;
	size_t			block_size;
	ut_new_pfx_t		crypt_pfx;
	row_merge_block_t*	crypt_block = NULL;
	ulint			i;
	ulint			j;
	dberr_t			error;
	pfs_os_file_t		tmpfd = OS_FILE_CLOSED;
	dict_index_t*		fts_sort_idx = NULL;
	fts_psort_t*		psort_info = NULL;
	fts_psort_t*		merge_info = NULL;
	bool			fts_psort_initiated = false;

	double total_static_cost = 0;
	double total_dynamic_cost = 0;
	ulint total_index_blocks = 0;
	double pct_cost=0;
	double pct_progress=0;

	DBUG_ENTER("row_merge_build_indexes");

	ut_ad(!srv_read_only_mode);
	ut_ad((old_table == new_table) == !col_map);
	ut_ad(!defaults || col_map);

	stage->begin_phase_read_pk(skip_pk_sort && new_table != old_table
				   ? n_indexes - 1
				   : n_indexes);

	/* Allocate memory for merge file data structure and initialize
	fields */

	ut_allocator<row_merge_block_t>	alloc(mem_key_row_merge_sort);

	/* This will allocate "3 * srv_sort_buf_size" elements of type
	row_merge_block_t. The latter is defined as byte. */
	block_size = 3 * srv_sort_buf_size;
	block = alloc.allocate_large(block_size, &block_pfx);

	if (block == NULL) {
		DBUG_RETURN(DB_OUT_OF_MEMORY);
	}

	crypt_pfx.m_size = 0; /* silence bogus -Wmaybe-uninitialized */
	TRASH_ALLOC(&crypt_pfx, sizeof crypt_pfx);

	if (srv_encrypt_log) {
		crypt_block = static_cast<row_merge_block_t*>(
			alloc.allocate_large(block_size,
					     &crypt_pfx));

		if (crypt_block == NULL) {
			DBUG_RETURN(DB_OUT_OF_MEMORY);
		}
	}

	trx_start_if_not_started_xa(trx, true);
	ulint	n_merge_files = 0;

	for (ulint i = 0; i < n_indexes; i++)
	{
		if (!dict_index_is_spatial(indexes[i])) {
			n_merge_files++;
		}
	}

	merge_files = static_cast<merge_file_t*>(
		ut_malloc_nokey(n_merge_files * sizeof *merge_files));

	/* Initialize all the merge file descriptors, so that we
	don't call row_merge_file_destroy() on uninitialized
	merge file descriptor */

	for (i = 0; i < n_merge_files; i++) {
		merge_files[i].fd = OS_FILE_CLOSED;
		merge_files[i].offset = 0;
		merge_files[i].n_rec = 0;
	}

	total_static_cost = COST_BUILD_INDEX_STATIC
		* static_cast<double>(n_indexes) + COST_READ_CLUSTERED_INDEX;
	total_dynamic_cost = COST_BUILD_INDEX_DYNAMIC
		* static_cast<double>(n_indexes);
	for (i = 0; i < n_indexes; i++) {
		if (indexes[i]->type & DICT_FTS) {
			ibool	opt_doc_id_size = FALSE;

			/* To build FTS index, we would need to extract
			doc's word, Doc ID, and word's position, so
			we need to build a "fts sort index" indexing
			on above three 'fields' */
			fts_sort_idx = row_merge_create_fts_sort_index(
				indexes[i], old_table, &opt_doc_id_size);

			row_merge_dup_t*	dup
				= static_cast<row_merge_dup_t*>(
					ut_malloc_nokey(sizeof *dup));
			dup->index = fts_sort_idx;
			dup->table = table;
			dup->col_map = col_map;
			dup->n_dup = 0;

			/* This can fail e.g. if temporal files can't be
			created */
			if (!row_fts_psort_info_init(
					trx, dup, new_table, opt_doc_id_size,
					old_table->space->zip_size(),
					&psort_info, &merge_info)) {
				error = DB_CORRUPTION;
				goto func_exit;
			}

			/* We need to ensure that we free the resources
			allocated */
			fts_psort_initiated = true;
		}
	}

	if (global_system_variables.log_warnings > 2) {
		sql_print_information("InnoDB: Online DDL : Start reading"
				      " clustered index of the table"
				      " and create temporary files");
	}

	pct_cost = COST_READ_CLUSTERED_INDEX * 100 / (total_static_cost + total_dynamic_cost);

	/* Do not continue if we can't encrypt table pages */
	if (!old_table->is_readable() ||
	    !new_table->is_readable()) {
		error = innodb_decryption_failed(trx->mysql_thd,
						 !old_table->is_readable()
						 ? old_table : new_table);
		goto func_exit;
	}

	/* Read clustered index of the table and create files for
	secondary index entries for merge sort */
	error = row_merge_read_clustered_index(
		trx, table, old_table, new_table, online, indexes,
		fts_sort_idx, psort_info, merge_files, key_numbers,
		n_indexes, defaults, add_v, col_map, add_autoinc,
		sequence, block, skip_pk_sort, &tmpfd, stage,
		pct_cost, crypt_block, eval_table, allow_not_null,
		col_collate);

	stage->end_phase_read_pk();

	pct_progress += pct_cost;

	if (global_system_variables.log_warnings > 2) {
		sql_print_information("InnoDB: Online DDL : End of reading "
				      "clustered index of the table"
				      " and create temporary files");
	}

	for (i = 0; i < n_merge_files; i++) {
		total_index_blocks += merge_files[i].offset;
	}

	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	DEBUG_SYNC_C("row_merge_after_scan");

	/* Now we have files containing index entries ready for
	sorting and inserting. */

	for (ulint k = 0, i = 0; i < n_indexes; i++) {
		dict_index_t*	sort_idx = indexes[i];

		if (dict_index_is_spatial(sort_idx)) {
			continue;
		}

		if (indexes[i]->type & DICT_FTS) {

			sort_idx = fts_sort_idx;

			if (FTS_PLL_MERGE) {
				row_fts_start_parallel_merge(merge_info);
				for (j = 0; j < FTS_NUM_AUX_INDEX; j++) {
					merge_info[j].task->wait();
					delete merge_info[j].task;
				}
			} else {
				/* This cannot report duplicates; an
				assertion would fail in that case. */
				error = row_fts_merge_insert(
					sort_idx, new_table,
					psort_info, 0);
			}

#ifdef FTS_INTERNAL_DIAG_PRINT
			DEBUG_FTS_SORT_PRINT("FTS_SORT: Complete Insert\n");
#endif
		} else if (merge_files[k].fd != OS_FILE_CLOSED) {
			char	buf[NAME_LEN + 1];
			row_merge_dup_t	dup = {
				sort_idx, table, col_map, 0};

			pct_cost = (COST_BUILD_INDEX_STATIC +
				    (total_dynamic_cost
				     * static_cast<double>(merge_files[k].offset)
				     / static_cast<double>(total_index_blocks)))
				/ (total_static_cost + total_dynamic_cost)
				* PCT_COST_MERGESORT_INDEX * 100;
			char*	bufend = innobase_convert_name(
				buf, sizeof buf,
				indexes[i]->name,
				strlen(indexes[i]->name),
				trx->mysql_thd);
			buf[bufend - buf]='\0';

			if (global_system_variables.log_warnings > 2) {
				sql_print_information("InnoDB: Online DDL :"
						      " Start merge-sorting"
						      " index %s"
						      " (" ULINTPF
						      " / " ULINTPF "),"
						      " estimated cost :"
						      " %2.4f",
						      buf, i + 1, n_indexes,
						      pct_cost);
			}

			error = row_merge_sort(
					trx, &dup, &merge_files[k],
					block, &tmpfd, true,
					pct_progress, pct_cost,
					crypt_block, new_table->space_id,
					stage);

			pct_progress += pct_cost;

			if (global_system_variables.log_warnings > 2) {
				sql_print_information("InnoDB: Online DDL :"
						      " End of "
						      " merge-sorting index %s"
						      " (" ULINTPF
						      " / " ULINTPF ")",
						      buf, i + 1, n_indexes);
			}

			if (error == DB_SUCCESS) {
				BtrBulk	btr_bulk(sort_idx, trx);

				pct_cost = (COST_BUILD_INDEX_STATIC +
					    (total_dynamic_cost
					     * static_cast<double>(
						     merge_files[k].offset)
					     / static_cast<double>(
						     total_index_blocks)))
					/ (total_static_cost
					   + total_dynamic_cost)
					* PCT_COST_INSERT_INDEX * 100;

				if (global_system_variables.log_warnings > 2) {
					sql_print_information(
						"InnoDB: Online DDL : Start "
						"building index %s"
						" (" ULINTPF
						" / " ULINTPF "), estimated "
						"cost : %2.4f", buf, i + 1,
						n_indexes, pct_cost);
				}

				error = row_merge_insert_index_tuples(
					sort_idx, old_table,
					merge_files[k].fd, block, NULL,
					&btr_bulk,
					merge_files[k].n_rec, pct_progress, pct_cost,
					crypt_block, new_table->space_id,
					stage);

				error = btr_bulk.finish(error);

				pct_progress += pct_cost;

				if (global_system_variables.log_warnings > 2) {
					sql_print_information(
						"InnoDB: Online DDL : "
						"End of building index %s"
						" (" ULINTPF " / " ULINTPF ")",
						buf, i + 1, n_indexes);
				}
			}
		}

		/* Close the temporary file to free up space. */
		row_merge_file_destroy(&merge_files[k++]);

		if (indexes[i]->type & DICT_FTS) {
			row_fts_psort_info_destroy(psort_info, merge_info);
			fts_psort_initiated = false;
		} else if (old_table != new_table) {
			ut_ad(!sort_idx->online_log);
			ut_ad(sort_idx->online_status
			      == ONLINE_INDEX_COMPLETE);
		}

		if (old_table != new_table
		    || (indexes[i]->type & (DICT_FTS | DICT_SPATIAL))
		    || error != DB_SUCCESS || !online) {
			/* Do not apply any online log. */
		} else {
			if (global_system_variables.log_warnings > 2) {
				sql_print_information(
					"InnoDB: Online DDL : Applying"
					" log to index");
			}

			DEBUG_SYNC_C("row_log_apply_before");
			error = row_log_apply(trx, sort_idx, table, stage);
			DEBUG_SYNC_C("row_log_apply_after");
		}

		if (error != DB_SUCCESS) {
			trx->error_key_num = key_numbers[i];
			goto func_exit;
		}

		if (indexes[i]->type & DICT_FTS
		    && UNIV_UNLIKELY(fts_enable_diag_print)) {
			ib::info() << "Finished building full-text index "
				<< indexes[i]->name;
		}
	}

func_exit:

	DBUG_EXECUTE_IF(
		"ib_build_indexes_too_many_concurrent_trxs",
		error = DB_TOO_MANY_CONCURRENT_TRXS;
		trx->error_state = error;);

	if (fts_psort_initiated) {
		/* Clean up FTS psort related resource */
		row_fts_psort_info_destroy(psort_info, merge_info);
		fts_psort_initiated = false;
	}

	row_merge_file_destroy_low(tmpfd);

	for (i = 0; i < n_merge_files; i++) {
		row_merge_file_destroy(&merge_files[i]);
	}

	if (fts_sort_idx) {
		dict_mem_index_free(fts_sort_idx);
	}

	ut_free(merge_files);

	alloc.deallocate_large(block, &block_pfx);

	if (crypt_block) {
		alloc.deallocate_large(crypt_block, &crypt_pfx);
	}

	DICT_TF2_FLAG_UNSET(new_table, DICT_TF2_FTS_ADD_DOC_ID);

	if (online && old_table == new_table && error != DB_SUCCESS) {
		/* On error, flag all online secondary index creation
		as aborted. */
		for (i = 0; i < n_indexes; i++) {
			ut_ad(!(indexes[i]->type & DICT_FTS));
			ut_ad(!indexes[i]->is_committed());
			ut_ad(!dict_index_is_clust(indexes[i]));

			/* Completed indexes should be dropped as
			well, and indexes whose creation was aborted
			should be dropped from the persistent
			storage. However, at this point we can only
			set some flags in the not-yet-published
			indexes. These indexes will be dropped later
			in row_merge_drop_indexes(), called by
			rollback_inplace_alter_table(). */

			switch (dict_index_get_online_status(indexes[i])) {
			case ONLINE_INDEX_COMPLETE:
				break;
			case ONLINE_INDEX_CREATION:
				indexes[i]->lock.x_lock(SRW_LOCK_CALL);
				row_log_abort_sec(indexes[i]);
				indexes[i]->type |= DICT_CORRUPT;
				indexes[i]->lock.x_unlock();
				new_table->drop_aborted = TRUE;
				/* fall through */
			case ONLINE_INDEX_ABORTED_DROPPED:
			case ONLINE_INDEX_ABORTED:
				MONITOR_ATOMIC_INC(
					MONITOR_BACKGROUND_DROP_INDEX);
			}
		}

		dict_index_t *clust_index= new_table->indexes.start;
		clust_index->lock.x_lock(SRW_LOCK_CALL);
		ut_ad(!clust_index->online_log ||
		      clust_index->online_log_is_dummy());
		clust_index->online_log= nullptr;
		clust_index->lock.x_unlock();
	}

	DBUG_RETURN(error);
}

dberr_t row_merge_bulk_t::alloc_block()
{
  if (m_block)
    return DB_SUCCESS;
  m_block= m_alloc.allocate_large_dontdump(
             3 * srv_sort_buf_size, &m_block_pfx);
  if (m_block == nullptr)
    return DB_OUT_OF_MEMORY;

  m_crypt_pfx.m_size= 0;
  TRASH_ALLOC(&m_crypt_pfx, sizeof m_crypt_pfx);
  if (srv_encrypt_log)
  {
    m_crypt_block= static_cast<row_merge_block_t*>(
       m_alloc.allocate_large(3 * srv_sort_buf_size, &m_crypt_pfx));
    if (!m_crypt_block)
      return DB_OUT_OF_MEMORY;
  }
  return DB_SUCCESS;
}

row_merge_bulk_t::row_merge_bulk_t(dict_table_t *table,
                                   bool sort_primary_key)
  : m_sort_primary_key(sort_primary_key)
{
  ulint n_index= 0;
  for (dict_index_t *index= UT_LIST_GET_FIRST(table->indexes);
       index; index= UT_LIST_GET_NEXT(indexes, index))
  {
    if (!index->is_btree())
      continue;
    n_index++;
  }

  m_merge_buf= static_cast<row_merge_buf_t*>(
    ut_zalloc_nokey(n_index * sizeof *m_merge_buf));

  ulint i= 0;
  for (dict_index_t *index= UT_LIST_GET_FIRST(table->indexes);
       index; index= UT_LIST_GET_NEXT(indexes, index))
  {
    if (!index->is_btree())
      continue;

    mem_heap_t *heap= mem_heap_create(100);
    row_merge_buf_create_low(&m_merge_buf[i], heap, index);
    i++;
  }

  m_tmpfd= OS_FILE_CLOSED;
  m_blob_file.fd= OS_FILE_CLOSED;
  m_blob_file.offset= 0;
  m_blob_file.n_rec= 0;
}

row_merge_bulk_t::~row_merge_bulk_t()
{
  ulint i= 0;
  dict_table_t *table= m_merge_buf[0].index->table;
  for (dict_index_t *index= UT_LIST_GET_FIRST(table->indexes);
       index; index= UT_LIST_GET_NEXT(indexes, index))
  {
    if (!index->is_btree())
      continue;
    row_merge_buf_free(&m_merge_buf[i]);
    if (m_merge_files)
      row_merge_file_destroy(&m_merge_files[i]);
    i++;
  }

  row_merge_file_destroy_low(m_tmpfd);

  row_merge_file_destroy(&m_blob_file);

  ut_free(m_merge_buf);

  ut_free(m_merge_files);

  if (m_block)
    m_alloc.deallocate_large(m_block, &m_block_pfx);

  if (m_crypt_block)
    m_alloc.deallocate_large(m_crypt_block, &m_crypt_pfx);
}

void row_merge_bulk_t::init_tmp_file()
{
  if (m_merge_files)
    return;

  ulint n_index= 0;
  dict_table_t *table= m_merge_buf[0].index->table;
  for (dict_index_t *index= UT_LIST_GET_FIRST(table->indexes);
       index; index= UT_LIST_GET_NEXT(indexes, index))
  {
    if (!index->is_btree())
      continue;
    n_index++;
  }

  m_merge_files= static_cast<merge_file_t*>(
    ut_malloc_nokey(n_index * sizeof *m_merge_files));

  for (ulint i= 0; i < n_index; i++)
  {
    m_merge_files[i].fd= OS_FILE_CLOSED;
    m_merge_files[i].offset= 0;
    m_merge_files[i].n_rec= 0;
  }
}

void row_merge_bulk_t::clean_bulk_buffer(ulint index_no)
{
  mem_heap_empty(m_merge_buf[index_no].heap);
  m_merge_buf[index_no].total_size = m_merge_buf[index_no].n_tuples = 0;
}

bool row_merge_bulk_t::create_tmp_file(ulint index_no)
{
  return row_merge_file_create_if_needed(
            &m_merge_files[index_no], &m_tmpfd,
            m_merge_buf[index_no].n_tuples, NULL);
}

dberr_t row_merge_bulk_t::write_to_tmp_file(ulint index_no)
{
  if (!create_tmp_file(index_no))
    return DB_OUT_OF_MEMORY;
  merge_file_t *file= &m_merge_files[index_no];
  row_merge_buf_t *buf= &m_merge_buf[index_no];

  alloc_block();

  if (dberr_t err= row_merge_buf_write(buf,
#ifndef DBUG_OFF
                                       file,
#endif
                                       m_block,
                                       index_no == 0 ? &m_blob_file : nullptr))
    return err;

  if (!row_merge_write(file->fd, file->offset++,
                       m_block, m_crypt_block,
                       buf->index->table->space->id))
    return DB_TEMP_FILE_WRITE_FAIL;

  DBUG_EXECUTE_IF("write_to_tmp_file_fail",
                  return DB_TEMP_FILE_WRITE_FAIL;);
  MEM_UNDEFINED(&m_block[0], srv_sort_buf_size);
  return DB_SUCCESS;
}

ATTRIBUTE_COLD
dberr_t row_merge_bulk_t::load_one_row(trx_t *trx)
{
  /* Load the single row into the clustered index. BtrBulk has
  nothing to do for bulk insert here and used only as a interface
  to insert single row. */
  dict_index_t *index= m_merge_buf[0].index;
  BtrBulk btr_bulk(index, trx);
  ut_ad(m_merge_buf[0].n_tuples == 1);
  dberr_t err= row_merge_insert_index_tuples(index, index->table,
                                             OS_FILE_CLOSED, nullptr,
                                             &m_merge_buf[0], &btr_bulk,
                                             0, 0, 0, nullptr,
                                             index->table->space_id,
                                             nullptr,
                                             m_blob_file.fd == OS_FILE_CLOSED
                                             ? nullptr : &m_blob_file);
  if (err != DB_SUCCESS)
    trx->error_info= index;
  else if (index->table->persistent_autoinc)
    btr_write_autoinc(index, 1);
  err= btr_bulk.finish(err);
  if (err == DB_SUCCESS && index->is_clust())
    index->table->stat_n_rows= 1;
  return err;
}

dberr_t row_merge_bulk_t::bulk_insert_buffered(const dtuple_t &row,
                                               const dict_index_t &ind,
                                               trx_t *trx)
{
  ut_ad(row.n_fields == ind.n_fields);
  dberr_t err= DB_SUCCESS;
  ulint i= 0;
  mem_heap_t *large_tuple_heap= nullptr;
  for (dict_index_t *index= UT_LIST_GET_FIRST(ind.table->indexes);
       index; index= UT_LIST_GET_NEXT(indexes, index))
  {
    if (!index->is_btree())
      continue;

    if (index != &ind)
    {
      i++;
      continue;
    }
    row_merge_buf_t *buf= &m_merge_buf[i];
add_to_buf:
    if (row_merge_bulk_buf_add(buf, *ind.table, row))
    {
      i++;
      goto func_exit;
    }

    if (buf->n_tuples == 0)
    {
      /* Tuple data size is greater than srv_sort_buf_size */
      ut_ad(i == 0);
      if (!large_tuple_heap)
        large_tuple_heap= mem_heap_create(DTUPLE_EST_ALLOC(row.n_fields));

      dtuple_t *big_tuple= dtuple_copy(&row, large_tuple_heap);
      err= row_merge_buf_blob(big_tuple->fields, &m_blob_file,
                              &ind, &large_tuple_heap);
      if (err)
	goto func_exit;

      if (row_merge_bulk_buf_add(buf, *ind.table, *big_tuple))
      {
        i++;
	goto func_exit;
      }
    }

    if (index->is_unique())
    {
      row_merge_dup_t dup{index, nullptr, nullptr, 0};
      row_merge_buf_sort(buf, &dup);
      if (dup.n_dup)
      {
        trx->error_info= index;
        err= DB_DUPLICATE_KEY;
        goto func_exit;
      }
    }
    else
      row_merge_buf_sort(buf, NULL);
    init_tmp_file();
    merge_file_t *file= &m_merge_files[i];
    file->n_rec+= buf->n_tuples;
    err= write_to_tmp_file(i);
    if (err != DB_SUCCESS)
    {
      trx->error_info= index;
      goto func_exit;
    }
    clean_bulk_buffer(i);
    buf= &m_merge_buf[i];
    goto add_to_buf;
  }

func_exit:
  if (!m_sort_primary_key && ind.is_clust())
    err= load_one_row(trx);
  if (large_tuple_heap)
    mem_heap_free(large_tuple_heap);
  return err;
}

dberr_t row_merge_bulk_t::write_to_index(ulint index_no, trx_t *trx)
{
  dberr_t err= DB_SUCCESS;
  row_merge_buf_t buf= m_merge_buf[index_no];
  merge_file_t *file= m_merge_files ?
    &m_merge_files[index_no] : nullptr;
  dict_index_t *index= buf.index;
  dict_table_t *table= index->table;
  BtrBulk btr_bulk(index, trx);
  row_merge_dup_t dup = {index, nullptr, nullptr, 0};

  if (buf.n_tuples)
  {
    if (dict_index_is_unique(index))
    {
      row_merge_buf_sort(&buf, &dup);
      if (dup.n_dup)
      {
        err= DB_DUPLICATE_KEY;
        goto func_exit;
      }
    }
    else row_merge_buf_sort(&buf, NULL);
    if (file && file->fd != OS_FILE_CLOSED)
    {
      file->n_rec+= buf.n_tuples;
      err= write_to_tmp_file(index_no);
      if (err!= DB_SUCCESS)
        goto func_exit;
    }
    else
    {
      /* Data got fit in merge buffer. */
      err= row_merge_insert_index_tuples(
            index, table, OS_FILE_CLOSED, nullptr,
            &buf, &btr_bulk, 0, 0, 0, nullptr, table->space_id, nullptr,
            m_blob_file.fd == OS_FILE_CLOSED ? nullptr : &m_blob_file);
      goto func_exit;
    }
  }

  err= row_merge_sort(trx, &dup, file,
                      m_block, &m_tmpfd, true, 0, 0,
                      m_crypt_block, table->space_id, nullptr);
  if (err != DB_SUCCESS)
    goto func_exit;

  err= row_merge_insert_index_tuples(
        index, table, file->fd, m_block, nullptr,
        &btr_bulk, 0, 0, 0, m_crypt_block, table->space_id,
        nullptr, &m_blob_file);

func_exit:
  if (err != DB_SUCCESS)
    trx->error_info= index;
  else if (index->is_primary() && table->persistent_autoinc)
    btr_write_autoinc(index, table->autoinc - 1);
  err= btr_bulk.finish(err);
  if (err == DB_SUCCESS && index->is_clust())
    table->stat_n_rows= (file && file->fd != OS_FILE_CLOSED)
      ? file->n_rec : buf.n_tuples;
  return err;
}

dberr_t row_merge_bulk_t::write_to_table(dict_table_t *table, trx_t *trx)
{
  dict_index_t *index= UT_LIST_GET_FIRST(table->indexes);
  ut_ad(index->is_clust());
  ulint i= !m_sort_primary_key;
  if (i)
    /* For clustered index, InnoDB does call load_one_row() while
    buffering the first insert and uses row_ins_clust_index_entry()
    for subsequent rows. So skip the clustered index while applying
    the buffered insert operation */
    index= UT_LIST_GET_NEXT(indexes, index);
  for (; index; index= UT_LIST_GET_NEXT(indexes, index))
  {
    if (!index->is_btree())
      continue;

    dberr_t err= write_to_index(i, trx);
    switch (err) {
    default:
      if (table->skip_alter_undo)
        my_error_innodb(err, table->name.m_name, table->flags);
      return err;
    case DB_SUCCESS:
      break;
    case DB_DUPLICATE_KEY:
      trx->error_info= index;
      return err;
    }
    i++;
  }

  return DB_SUCCESS;
}

dberr_t trx_mod_table_time_t::write_bulk(dict_table_t *table, trx_t *trx)
{
  if (!bulk_store)
    return DB_SUCCESS;
  dberr_t err= bulk_store->write_to_table(table, trx);
  delete bulk_store;
  bulk_store= nullptr;
  return err;
}

void trx_t::bulk_rollback_low()
{
  undo_no_t low_limit= UINT64_MAX;
  for (auto& t : mod_tables)
  {
    if (t.second.is_bulk_insert())
    {
      if (t.second.get_first() < low_limit)
        low_limit= t.second.get_first();
      delete t.second.bulk_store;
      t.second.bulk_store= nullptr;
      t.second.end_bulk_insert();
    }
  }
  rollback(&low_limit);
}

dberr_t trx_t::bulk_insert_apply_low()
{
  ut_ad(bulk_insert);
  for (auto& t : mod_tables)
    if (t.second.is_bulk_insert())
      if (dberr_t err= t.second.write_bulk(t.first, this))
      {
        bulk_rollback_low();
        return err;
      }
  return DB_SUCCESS;
}
