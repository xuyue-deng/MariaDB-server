/*
   Copyright (c) 2000, 2018, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_plugin.h"
#include "myisamdef.h"
#include "sql_priv.h"
#include "key.h"                                // key_copy
#include <m_ctype.h>
#include <my_bit.h>
#include "ha_myisam.h"
#include "rt_index.h"
#include "sql_table.h"                          // tablename_to_filename
#include "sql_class.h"                          // THD
#include "debug_sync.h"
#include "sql_debug.h"

ulonglong myisam_recover_options;
static ulong opt_myisam_block_size;

/* bits in myisam_recover_options */
const char *myisam_recover_names[] =
{ "DEFAULT", "BACKUP", "FORCE", "QUICK", "BACKUP_ALL", "OFF", NullS};
TYPELIB myisam_recover_typelib= CREATE_TYPELIB_FOR(myisam_recover_names);

const char *myisam_stats_method_names[] = {"NULLS_UNEQUAL", "NULLS_EQUAL",
                                           "NULLS_IGNORED", NullS};
TYPELIB myisam_stats_method_typelib=
                CREATE_TYPELIB_FOR(myisam_stats_method_names);

static MYSQL_SYSVAR_ULONG(block_size, opt_myisam_block_size,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
  "Block size to be used for MyISAM index pages", NULL, NULL,
  MI_KEY_BLOCK_LENGTH, MI_MIN_KEY_BLOCK_LENGTH, MI_MAX_KEY_BLOCK_LENGTH,
  MI_MIN_KEY_BLOCK_LENGTH);

static MYSQL_SYSVAR_ULONG(data_pointer_size, myisam_data_pointer_size,
  PLUGIN_VAR_RQCMDARG, "Default pointer size to be used for MyISAM tables",
  NULL, NULL, 6, 2, 7, 1);

#define MB (1024*1024)
static MYSQL_SYSVAR_ULONGLONG(max_sort_file_size, myisam_max_temp_length,
  PLUGIN_VAR_RQCMDARG, "Don't use the fast sort index method to created "
  "index if the temporary file would get bigger than this", NULL, NULL,
  LONG_MAX/MB*MB, 0, MAX_FILE_SIZE, MB);

static MYSQL_SYSVAR_SET(recover_options, myisam_recover_options,
  PLUGIN_VAR_OPCMDARG|PLUGIN_VAR_READONLY,
  "Specifies how corrupted tables should be automatically repaired",
  NULL, NULL, HA_RECOVER_BACKUP|HA_RECOVER_QUICK, &myisam_recover_typelib);

static MYSQL_THDVAR_ULONG(repair_threads, PLUGIN_VAR_RQCMDARG,
  "If larger than 1, when repairing a MyISAM table all indexes will be "
  "created in parallel, with one thread per index. The value of 1 "
  "disables parallel repair", NULL, NULL,
  1, 1, ULONG_MAX, 1);

static MYSQL_THDVAR_ULONGLONG(sort_buffer_size, PLUGIN_VAR_RQCMDARG,
  "The buffer that is allocated when sorting the index when doing "
  "a REPAIR or when creating indexes with CREATE INDEX or ALTER TABLE", NULL, NULL,
  SORT_BUFFER_INIT, MIN_SORT_BUFFER, SIZE_T_MAX/16, 1);

static MYSQL_SYSVAR_BOOL(use_mmap, opt_myisam_use_mmap, PLUGIN_VAR_NOCMDARG,
  "Use memory mapping for reading and writing MyISAM tables", NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONGLONG(mmap_size, myisam_mmap_size,
  PLUGIN_VAR_RQCMDARG|PLUGIN_VAR_READONLY, "Restricts the total memory "
  "used for memory mapping of MyISAM tables", NULL, NULL,
  SIZE_T_MAX, MEMMAP_EXTRA_MARGIN, SIZE_T_MAX, 1);

static MYSQL_THDVAR_ENUM(stats_method, PLUGIN_VAR_RQCMDARG,
  "Specifies how MyISAM index statistics collection code should "
  "treat NULLs. Possible values of name are NULLS_UNEQUAL (default "
  "behavior for 4.1 and later), NULLS_EQUAL (emulate 4.0 behavior), "
  "and NULLS_IGNORED", NULL, NULL,
  MI_STATS_METHOD_NULLS_NOT_EQUAL, &myisam_stats_method_typelib);

const LEX_CSTRING MI_CHECK_INFO= { STRING_WITH_LEN("info") };
const LEX_CSTRING MI_CHECK_WARNING= { STRING_WITH_LEN("warning") };
const LEX_CSTRING MI_CHECK_ERROR= { STRING_WITH_LEN("error") };

#ifndef DBUG_OFF
/**
  Causes the thread to wait in a spin lock for a query kill signal.
  This function is used by the test frame work to identify race conditions.

  The signal is caught and ignored and the thread is not killed.
*/

static void debug_wait_for_kill(const char *info)
{
  DBUG_ENTER("debug_wait_for_kill");
  const char *prev_info;
  THD *thd;
  thd= current_thd;
  prev_info= thd_proc_info(thd, info);
  while(!thd->killed)
    my_sleep(1000);
  DBUG_PRINT("info", ("Exit debug_wait_for_kill"));
  thd_proc_info(thd, prev_info);
  DBUG_VOID_RETURN;
}


class Debug_key_myisam: public Debug_key
{
public:
  Debug_key_myisam() { }

  static void print_keys_myisam(THD *thd, const char *where,
                                const TABLE *table,
                                const MI_KEYDEF *keydef, uint count)
  {
    for (uint i= 0; i < count; i++)
    {
      Debug_key_myisam tmp;
      if (!tmp.append(where, strlen(where)) &&
          !tmp.append_key(table->s->key_info[i].name, keydef[i].flag))
        tmp.print(thd);
      print_keysegs(thd, keydef[i].seg, keydef[i].keysegs);
    }
  }
};

#endif

/*****************************************************************************
** MyISAM tables
*****************************************************************************/

static handler *myisam_create_handler(handlerton *hton,
                                      TABLE_SHARE *table, 
                                      MEM_ROOT *mem_root)
{
  return new (mem_root) ha_myisam(hton, table);
}


static void mi_check_print(HA_CHECK *param, const LEX_CSTRING* msg_type,
                           const char *msgbuf)
{
  if (msg_type == &MI_CHECK_INFO)
    sql_print_information("%s.%s: %s", param->db_name, param->table_name,
                          msgbuf);
  else if (msg_type == &MI_CHECK_WARNING)
    sql_print_warning("%s.%s: %s", param->db_name, param->table_name,
                      msgbuf);
  else
    sql_print_error("%s.%s: %s", param->db_name, param->table_name, msgbuf);
}

// collect errors printed by mi_check routines

ATTRIBUTE_FORMAT(printf, 3, 0)
static void mi_check_print_msg(HA_CHECK *param,	const LEX_CSTRING *msg_type,
			       const char *fmt, va_list args)
{
  THD* thd = (THD*)param->thd;
  Protocol *protocol= thd->protocol;
  size_t length, msg_length;
  char msgbuf[MYSQL_ERRMSG_SIZE];
  char name[NAME_LEN*2+2];

  if (param->testflag & T_SUPPRESS_ERR_HANDLING)
    return;

  msg_length= my_vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
  msgbuf[sizeof(msgbuf) - 1] = 0; // healthy paranoia

  DBUG_PRINT(msg_type->str,("message: %s",msgbuf));

  if (!thd->vio_ok())
  {
    mi_check_print(param, msg_type, msgbuf);
    return;
  }

  if (param->testflag & (T_CREATE_MISSING_KEYS | T_SAFE_REPAIR |
			 T_AUTO_REPAIR))
  {
    myf flag= 0;
    if (msg_type == &MI_CHECK_INFO)
      flag= ME_NOTE;
    else if (msg_type == &MI_CHECK_WARNING)
      flag= ME_WARNING;
    my_message(ER_NOT_KEYFILE, msgbuf, MYF(flag));
    if (thd->variables.log_warnings > 2 && ! thd->log_all_errors)
      mi_check_print(param, msg_type, msgbuf);
    return;
  }
  length=(uint) (strxmov(name, param->db_name,".",param->table_name,NullS) -
		 name);
  /*
    TODO: switch from protocol to push_warning here. The main reason we didn't
    it yet is parallel repair, which threads have no THD object accessible via
    current_thd.

    Also we likely need to lock mutex here (in both cases with protocol and
    push_warning).
  */
  if (param->need_print_msg_lock)
    mysql_mutex_lock(&param->print_msg_mutex);

  protocol->prepare_for_resend();
  protocol->store(name, length, system_charset_info);
  protocol->store(param->op_name, strlen(param->op_name), system_charset_info);
  protocol->store(msg_type, system_charset_info);
  protocol->store(msgbuf, msg_length, system_charset_info);
  if (protocol->write())
    sql_print_error("Failed on my_net_write, writing to stderr instead: %s\n",
		    msgbuf);
  else if (thd->variables.log_warnings > 2)
    mi_check_print(param, msg_type, msgbuf);

  if (param->need_print_msg_lock)
    mysql_mutex_unlock(&param->print_msg_mutex);

  return;
}


/*
  Convert TABLE object to MyISAM key and column definition

  SYNOPSIS
    table2myisam()
      table_arg   in     TABLE object.
      keydef_out  out    MyISAM key definition.
      recinfo_out out    MyISAM column definition.
      records_out out    Number of fields.

  DESCRIPTION
    This function will allocate and initialize MyISAM key and column
    definition for further use in mi_create or for a check for underlying
    table conformance in merge engine.

    The caller needs to free *recinfo_out after use. Since *recinfo_out
    and *keydef_out are allocated with a my_multi_malloc, *keydef_out
    is freed automatically when *recinfo_out is freed.

  RETURN VALUE
    0  OK
    !0 error code
*/

int table2myisam(TABLE *table_arg, MI_KEYDEF **keydef_out,
                 MI_COLUMNDEF **recinfo_out, uint *records_out)
{
  uint i, j, recpos, minpos, fieldpos, temp_length, length;
  enum ha_base_keytype type= HA_KEYTYPE_BINARY;
  uchar *record;
  KEY *pos;
  MI_KEYDEF *keydef;
  MI_COLUMNDEF *recinfo, *recinfo_pos;
  HA_KEYSEG *keyseg;
  TABLE_SHARE *share= table_arg->s;
  uint options= share->db_options_in_use;
  DBUG_ENTER("table2myisam");
  if (!(my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
          recinfo_out, (share->fields * 2 + 2) * sizeof(MI_COLUMNDEF),
          keydef_out, share->keys * sizeof(MI_KEYDEF),
          &keyseg,
          (share->key_parts + share->keys) * sizeof(HA_KEYSEG),
          NullS)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM); /* purecov: inspected */
  keydef= *keydef_out;
  recinfo= *recinfo_out;
  pos= table_arg->key_info;
  for (i= 0; i < share->keys; i++, pos++)
  {
    keydef[i].flag= ((uint16) pos->flags & (HA_NOSAME | HA_FULLTEXT_legacy
                                            | HA_SPATIAL_legacy));
    keydef[i].key_alg= pos->algorithm == HA_KEY_ALG_UNDEF ? HA_KEY_ALG_BTREE
                                                          : pos->algorithm;
    keydef[i].block_length= pos->block_size;
    keydef[i].seg= keyseg;
    keydef[i].keysegs= pos->user_defined_key_parts;
    for (j= 0; j < pos->user_defined_key_parts; j++)
    {
      Field *field= pos->key_part[j].field;
      type= field->key_type();
      keydef[i].seg[j].flag= pos->key_part[j].key_part_flag;

      if (options & HA_OPTION_PACK_KEYS ||
          (pos->flags & (HA_PACK_KEY | HA_BINARY_PACK_KEY |
                         HA_SPACE_PACK_USED)))
      {
        if (pos->key_part[j].length > 8 &&
            (type == HA_KEYTYPE_TEXT ||
             type == HA_KEYTYPE_NUM ||
             (type == HA_KEYTYPE_BINARY && !field->zero_pack())))
        {
          /* No blobs here */
          if (j == 0)
            keydef[i].flag|= HA_PACK_KEY;
          if (!(field->flags & ZEROFILL_FLAG) &&
              (field->type() == MYSQL_TYPE_STRING ||
               field->type() == MYSQL_TYPE_VAR_STRING ||
               ((int) (pos->key_part[j].length - field->decimals())) >= 4))
            keydef[i].seg[j].flag|= HA_SPACE_PACK;
        }
        else if (j == 0 && (!(pos->flags & HA_NOSAME) || pos->key_length > 16))
          keydef[i].flag|= HA_BINARY_PACK_KEY;
      }
      keydef[i].seg[j].type= (int) type;
      keydef[i].seg[j].start= pos->key_part[j].offset;
      keydef[i].seg[j].length= pos->key_part[j].length;
      keydef[i].seg[j].bit_start= keydef[i].seg[j].bit_length= 0;
      keydef[i].seg[j].bit_pos= 0;
      keydef[i].seg[j].language= field->charset_for_protocol()->number;

      if (field->null_ptr)
      {
        keydef[i].seg[j].null_bit= field->null_bit;
        keydef[i].seg[j].null_pos= (uint) (field->null_ptr-
                                           (uchar*) table_arg->record[0]);
      }
      else
      {
        keydef[i].seg[j].null_bit= 0;
        keydef[i].seg[j].null_pos= 0;
      }
      if (field->type() == MYSQL_TYPE_BLOB ||
          field->type() == MYSQL_TYPE_GEOMETRY)
      {
        keydef[i].seg[j].flag|= HA_BLOB_PART;
        /* save number of bytes used to pack length */
        keydef[i].seg[j].bit_start= (uint) (field->pack_length() -
                                            portable_sizeof_char_ptr);
      }
      else if (field->type() == MYSQL_TYPE_BIT)
      {
        keydef[i].seg[j].bit_length= ((Field_bit *) field)->bit_len;
        keydef[i].seg[j].bit_start= ((Field_bit *) field)->bit_ofs;
        keydef[i].seg[j].bit_pos= (uint) (((Field_bit *) field)->bit_ptr -
                                          (uchar*) table_arg->record[0]);
      }
    }
    keyseg+= pos->user_defined_key_parts;
  }
  if (table_arg->found_next_number_field)
    keydef[share->next_number_index].flag|= HA_AUTO_KEY;
  record= table_arg->record[0];
  recpos= 0;
  recinfo_pos= recinfo;
  while (recpos < (uint) share->stored_rec_length)
  {
    Field **field, *found= 0;
    minpos= share->stored_rec_length;
    length= 0;

    for (field= table_arg->field; *field; field++)
    {
      if ((fieldpos= (*field)->offset(record)) >= recpos &&
          fieldpos < minpos)
      {
        /* skip null fields */
        if (!(temp_length= (*field)->pack_length_in_rec()))
          continue; /* Skip null-fields */
        if (! found || fieldpos < minpos ||
            (fieldpos == minpos && temp_length < length))
        {
          minpos= fieldpos;
          found= *field;
          length= temp_length;
        }
      }
    }
    DBUG_PRINT("loop", ("found: %p  recpos: %d  minpos: %d  length: %d",
                        found, recpos, minpos, length));
    if (recpos != minpos)
    {
      /* reserve space for null bits */
      bzero((char*) recinfo_pos, sizeof(*recinfo_pos));
      recinfo_pos->type= FIELD_NORMAL;
      recinfo_pos++->length= (uint16) (minpos - recpos);
    }
    if (!found)
      break;

    if (found->flags & BLOB_FLAG)
      recinfo_pos->type= FIELD_BLOB;
    else if (found->real_type() == MYSQL_TYPE_TIMESTAMP)
    {
      /* pre-MySQL-5.6.4 TIMESTAMP, or MariaDB-5.3+ TIMESTAMP */
      recinfo_pos->type= FIELD_NORMAL;
    }
    else if (found->type() == MYSQL_TYPE_VARCHAR)
      recinfo_pos->type= FIELD_VARCHAR;
    else if (!(options & HA_OPTION_PACK_RECORD))
      recinfo_pos->type= FIELD_NORMAL;
    else if (found->real_type() == MYSQL_TYPE_TIMESTAMP2)
    {
      /*
        MySQL-5.6.4+ erroneously marks Field_timestampf as FIELD_SKIP_PRESPACE,
        but only if HA_OPTION_PACK_RECORD is set.
      */
      recinfo_pos->type= FIELD_SKIP_PRESPACE;
    }
    else if (found->zero_pack())
      recinfo_pos->type= FIELD_SKIP_ZERO;
    else
      recinfo_pos->type= ((length <= 3 ||
                           (found->flags & ZEROFILL_FLAG)) ?
                          FIELD_NORMAL :
                          found->type() == MYSQL_TYPE_STRING ||
                          found->type() == MYSQL_TYPE_VAR_STRING ?
                          FIELD_SKIP_ENDSPACE :
                          FIELD_SKIP_PRESPACE);
    if (found->null_ptr)
    {
      recinfo_pos->null_bit= found->null_bit;
      recinfo_pos->null_pos= (uint) (found->null_ptr -
                                     (uchar*) table_arg->record[0]);
    }
    else
    {
      recinfo_pos->null_bit= 0;
      recinfo_pos->null_pos= 0;
    }
    (recinfo_pos++)->length= (uint16) length;
    recpos= minpos + length;
    DBUG_PRINT("loop", ("length: %d  type: %d",
                        recinfo_pos[-1].length,recinfo_pos[-1].type));
  }
  *records_out= (uint) (recinfo_pos - recinfo);
  DBUG_RETURN(0);
}


/*
  Check for underlying table conformance

  SYNOPSIS
    myisam_check_definition()
      t1_keyinfo       in    First table key definition
      t1_recinfo       in    First table record definition
      t1_keys          in    Number of keys in first table
      t1_recs          in    Number of records in first table
      t2_keyinfo       in    Second table key definition
      t2_recinfo       in    Second table record definition
      t2_keys          in    Number of keys in second table
      t2_recs          in    Number of records in second table
      strict           in    Strict check switch
      table            in    handle to the table object

  DESCRIPTION
    This function compares two MyISAM definitions. By intention it was done
    to compare merge table definition against underlying table definition.
    It may also be used to compare dot-frm and MYI definitions of MyISAM
    table as well to compare different MyISAM table definitions.

    For merge table it is not required that number of keys in merge table
    must exactly match number of keys in underlying table. When calling this
    function for underlying table conformance check, 'strict' flag must be
    set to false, and converted merge definition must be passed as t1_*.

    Otherwise 'strict' flag must be set to 1 and it is not required to pass
    converted dot-frm definition as t1_*.

    For compatibility reasons we relax some checks, specifically:
    - 4.0 (and earlier versions) always set key_alg to 0.
    - 4.0 (and earlier versions) have the same language for all keysegs.

  RETURN VALUE
    0 - Equal definitions.
    1 - Different definitions.

  TODO
    - compare FULLTEXT keys;
    - compare SPATIAL keys;
    - compare FIELD_SKIP_ZERO which is converted to FIELD_NORMAL correctly
      (should be correctly detected in table2myisam).
*/

int check_definition(MI_KEYDEF *t1_keyinfo, MI_COLUMNDEF *t1_recinfo,
                     uint t1_keys, uint t1_recs,
                     MI_KEYDEF *t2_keyinfo, MI_COLUMNDEF *t2_recinfo,
                     uint t2_keys, uint t2_recs, bool strict, TABLE *table_arg)
{
  uint i, j;
  DBUG_ENTER("check_definition");
  my_bool mysql_40_compat= table_arg && table_arg->s->frm_version < FRM_VER_TRUE_VARCHAR;
  if ((strict ? t1_keys != t2_keys : t1_keys > t2_keys))
  {
    DBUG_PRINT("error", ("Number of keys differs: t1_keys=%u, t2_keys=%u",
                         t1_keys, t2_keys));
    DBUG_RETURN(1);
  }
  if (t1_recs != t2_recs)
  {
    DBUG_PRINT("error", ("Number of recs differs: t1_recs=%u, t2_recs=%u",
                         t1_recs, t2_recs));
    DBUG_RETURN(1);
  }
  for (i= 0; i < t1_keys; i++)
  {
    HA_KEYSEG *t1_keysegs= t1_keyinfo[i].seg;
    HA_KEYSEG *t2_keysegs= t2_keyinfo[i].seg;
    if ((t1_keyinfo[i].key_alg == HA_KEY_ALG_FULLTEXT &&
         t2_keyinfo[i].key_alg == HA_KEY_ALG_FULLTEXT) ||
        (t1_keyinfo[i].key_alg == HA_KEY_ALG_RTREE &&
         t2_keyinfo[i].key_alg == HA_KEY_ALG_RTREE))
      continue;
    if ((!mysql_40_compat &&
        t1_keyinfo[i].key_alg != t2_keyinfo[i].key_alg) ||
        t1_keyinfo[i].keysegs != t2_keyinfo[i].keysegs)
    {
      DBUG_PRINT("error", ("Key %d has different definition", i));
      DBUG_PRINT("error", ("t1_keysegs=%d, t1_key_alg=%d",
                           t1_keyinfo[i].keysegs, t1_keyinfo[i].key_alg));
      DBUG_PRINT("error", ("t2_keysegs=%d, t2_key_alg=%d",
                           t2_keyinfo[i].keysegs, t2_keyinfo[i].key_alg));
      DBUG_RETURN(1);
    }
    for (j=  t1_keyinfo[i].keysegs; j--;)
    {
      uint8 t1_keysegs_j__type= t1_keysegs[j].type;

      /*
        Table migration from 4.1 to 5.1. In 5.1 a *TEXT key part is
        always HA_KEYTYPE_VARTEXT2. In 4.1 we had only the equivalent of
        HA_KEYTYPE_VARTEXT1. Since we treat both the same on MyISAM
        level, we can ignore a mismatch between these types.
      */
      if ((t1_keysegs[j].flag & HA_BLOB_PART) &&
          (t2_keysegs[j].flag & HA_BLOB_PART))
      {
        if ((t1_keysegs_j__type == HA_KEYTYPE_VARTEXT2) &&
            (t2_keysegs[j].type == HA_KEYTYPE_VARTEXT1))
          t1_keysegs_j__type= HA_KEYTYPE_VARTEXT1; /* purecov: tested */
        else if ((t1_keysegs_j__type == HA_KEYTYPE_VARBINARY2) &&
                 (t2_keysegs[j].type == HA_KEYTYPE_VARBINARY1))
          t1_keysegs_j__type= HA_KEYTYPE_VARBINARY1; /* purecov: inspected */
      }

      if ((!mysql_40_compat &&
          t1_keysegs[j].language != t2_keysegs[j].language) ||
          t1_keysegs_j__type != t2_keysegs[j].type ||
          t1_keysegs[j].null_bit != t2_keysegs[j].null_bit ||
          t1_keysegs[j].length != t2_keysegs[j].length ||
          t1_keysegs[j].start != t2_keysegs[j].start ||
          (t1_keysegs[j].flag ^ t2_keysegs[j].flag) & HA_REVERSE_SORT)
      {
        DBUG_PRINT("error", ("Key segment %d (key %d) has different "
                             "definition", j, i));
        DBUG_PRINT("error", ("t1_type=%d, t1_language=%d, t1_null_bit=%d, "
                             "t1_length=%d",
                             t1_keysegs[j].type, t1_keysegs[j].language,
                             t1_keysegs[j].null_bit, t1_keysegs[j].length));
        DBUG_PRINT("error", ("t2_type=%d, t2_language=%d, t2_null_bit=%d, "
                             "t2_length=%d",
                             t2_keysegs[j].type, t2_keysegs[j].language,
                             t2_keysegs[j].null_bit, t2_keysegs[j].length));

        DBUG_RETURN(1);
      }
    }
  }
  for (i= 0; i < t1_recs; i++)
  {
    MI_COLUMNDEF *t1_rec= &t1_recinfo[i];
    MI_COLUMNDEF *t2_rec= &t2_recinfo[i];
    /*
      FIELD_SKIP_ZERO can be changed to FIELD_NORMAL in mi_create,
      see NOTE1 in mi_create.c
    */
    if ((t1_rec->type != t2_rec->type &&
         !(t1_rec->type == (int) FIELD_SKIP_ZERO &&
           t1_rec->length == 1 &&
           t2_rec->type == (int) FIELD_NORMAL)) ||
        t1_rec->length != t2_rec->length ||
        t1_rec->null_bit != t2_rec->null_bit)
    {
      DBUG_PRINT("error", ("Field %d has different definition", i));
      DBUG_PRINT("error", ("t1_type=%d, t1_length=%d, t1_null_bit=%d",
                           t1_rec->type, t1_rec->length, t1_rec->null_bit));
      DBUG_PRINT("error", ("t2_type=%d, t2_length=%d, t2_null_bit=%d",
                           t2_rec->type, t2_rec->length, t2_rec->null_bit));
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

extern "C" {

int killed_ptr(HA_CHECK *param)
{
  if (likely(thd_killed((THD*)param->thd)) == 0)
    return 0;
  my_errno= HA_ERR_ABORTED_BY_USER;
  return 1;
}

void mi_check_print_error(HA_CHECK *param, const char *fmt,...)
{
  param->error_printed|=1;
  param->out_flag|= O_DATA_LOST;
  if (param->testflag & T_SUPPRESS_ERR_HANDLING)
    return;
  va_list args;
  va_start(args, fmt);
  mi_check_print_msg(param, &MI_CHECK_ERROR, fmt, args);
  va_end(args);
}

void mi_check_print_info(HA_CHECK *param, const char *fmt,...)
{
  va_list args;
  va_start(args, fmt);
  mi_check_print_msg(param, &MI_CHECK_INFO, fmt, args);
  param->note_printed= 1;
  va_end(args);
}

void mi_check_print_warning(HA_CHECK *param, const char *fmt,...)
{
  param->warning_printed=1;
  param->out_flag|= O_DATA_LOST;
  va_list args;
  va_start(args, fmt);
  mi_check_print_msg(param, &MI_CHECK_WARNING, fmt, args);
  va_end(args);
}


/**
  Report list of threads (and queries) accessing a table, thread_id of a
  thread that detected corruption, source file name and line number where
  this corruption was detected, optional extra information (string).

  This function is intended to be used when table corruption is detected.

  @param[in] file      MI_INFO object.
  @param[in] message   Optional error message.
  @param[in] sfile     Name of source file.
  @param[in] sline     Line number in source file.

  @return void
*/

void _mi_report_crashed(MI_INFO *file, const char *message,
                        const char *sfile, uint sline)
{
  THD *cur_thd;
  LIST *element;
  char buf[1024];
  mysql_mutex_lock(&file->s->intern_lock);
  if ((cur_thd= (THD*) file->in_use.data))
    sql_print_error("Got an error from thread_id=%lld, %s:%d", cur_thd->thread_id,
                    sfile, sline);
  else
    sql_print_error("Got an error from unknown thread, %s:%d", sfile, sline);
  if (message)
    sql_print_error("%s", message);
  for (element= file->s->in_use; element; element= list_rest(element))
  {
    THD *thd= (THD*) element->data;
    sql_print_error("%s",
                    thd ?
                    thd_get_error_context_description(thd, buf, sizeof(buf), 0)
                    : "Unknown thread accessing table");
  }
  mysql_mutex_unlock(&file->s->intern_lock);
}

/* Return 1 if user have requested query to be killed */

my_bool mi_killed_in_mariadb(MI_INFO *info)
{
  return (((TABLE*) (info->external_ref))->in_use->killed != 0);
}

static int compute_vcols(MI_INFO *info, uchar *record, int keynum)
{
  int error= 0;
  /* This mutex is needed for parallel repair */
  mysql_mutex_lock(&info->s->intern_lock);
  TABLE *table= (TABLE*)(info->external_ref);
  table->move_fields(table->field, record, table->record[0]);
  if (keynum == -1) // update all vcols
  {
    error= table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_READ);
    if (table->update_virtual_fields(table->file, VCOL_UPDATE_INDEXED))
      error= 1;
  }
  else
  {
    // update only one key
    KEY *key= table->key_info + keynum;
    KEY_PART_INFO *kp= key->key_part, *end= kp + key->ext_key_parts;
    for (; kp < end; kp++)
    {
      Field *f= table->field[kp->fieldnr - 1];
      if (f->vcol_info && !f->vcol_info->is_stored())
        table->update_virtual_field(f, false);
    }
  }
  table->move_fields(table->field, table->record[0], record);
  mysql_mutex_unlock(&info->s->intern_lock);
  return error;
}

}

ha_myisam::ha_myisam(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg), file(0),
  int_table_flags(HA_NULL_IN_KEY | HA_CAN_FULLTEXT | HA_CAN_SQL_HANDLER |
                  HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
                  HA_CAN_VIRTUAL_COLUMNS | HA_CAN_EXPORT |
                  HA_REQUIRES_KEY_COLUMNS_FOR_DELETE |
                  HA_DUPLICATE_POS | HA_CAN_INDEX_BLOBS | HA_AUTO_PART_KEY |
                  HA_FILE_BASED | HA_CAN_GEOMETRY | HA_NO_TRANSACTIONS |
                  HA_CAN_INSERT_DELAYED | HA_CAN_BIT_FIELD | HA_CAN_RTREEKEYS |
                  HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT | HA_CAN_REPAIR |
                  HA_CAN_TABLES_WITHOUT_ROLLBACK),
   can_enable_indexes(0)
{}

handler *ha_myisam::clone(const char *name __attribute__((unused)),
                          MEM_ROOT *mem_root)
{
  ha_myisam *new_handler=
    static_cast <ha_myisam *>(handler::clone(file->filename, mem_root));
  if (new_handler)
    new_handler->file->state= file->state;
  return new_handler;
}


static const char *ha_myisam_exts[] = {
  ".MYI",
  ".MYD",
  NullS
};

ulong ha_myisam::index_flags(uint inx, uint part, bool all_parts) const
{
  ulong flags;
  if (table_share->key_info[inx].algorithm == HA_KEY_ALG_FULLTEXT)
    flags= 0;
  else 
  if (table_share->key_info[inx].algorithm == HA_KEY_ALG_RTREE)
  {
    /* All GIS scans are non-ROR scans. We also disable IndexConditionPushdown */
    flags= HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE |
           HA_READ_ORDER | HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR;
  }
  else 
  {
    flags= HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE |
           HA_READ_ORDER | HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN |
           HA_DO_RANGE_FILTER_PUSHDOWN;
  }
  return flags;
}

IO_AND_CPU_COST ha_myisam::rnd_pos_time(ha_rows rows)
{
  IO_AND_CPU_COST cost= handler::rnd_pos_time(rows);
  /*
    Row data is not cached. costs.row_lookup_cost includes the cost of
    the reading the row from system (probably cached by the OS).
  */
  cost.io= 0;
  return cost;
}


/* Name is here without an extension */
int ha_myisam::open(const char *name, int mode, uint test_if_locked)
{
  MI_KEYDEF *keyinfo;
  MI_COLUMNDEF *recinfo= 0;
  char readlink_buf[FN_REFLEN], name_buff[FN_REFLEN];
  uint recs;
  uint i;

  /*
    If the user wants to have memory mapped data files, add an
    open_flag. Do not memory map temporary tables because they are
    expected to be inserted and thus extended a lot. Memory mapping is
    efficient for files that keep their size, but very inefficient for
    growing files. Using an open_flag instead of calling mi_extra(...
    HA_EXTRA_MMAP ...) after mi_open() has the advantage that the
    mapping is not repeated for every open, but just done on the initial
    open, when the MyISAM share is created. Every time the server
    requires opening a new instance of a table it calls this method. We
    will always supply HA_OPEN_MMAP for a permanent table. However, the
    MyISAM storage engine will ignore this flag if this is a secondary
    open of a table that is in use by other threads already (if the
    MyISAM share exists already).
  */
  if (!(test_if_locked & HA_OPEN_TMP_TABLE) && opt_myisam_use_mmap)
    test_if_locked|= HA_OPEN_MMAP;

  if (!(file=mi_open(name, mode, test_if_locked | HA_OPEN_FROM_SQL_LAYER)))
    return (my_errno ? my_errno : -1);

  file->s->chst_invalidator= query_cache_invalidate_by_MyISAM_filename_ref;
  /* Set external_ref, mainly for temporary tables */
  file->external_ref= (void*) table;            // For mi_killed()

  /* No need to perform a check for tmp table or if it's already checked */
  if (!table->s->tmp_table && file->s->reopen == 1)
  {
    if ((my_errno= table2myisam(table, &keyinfo, &recinfo, &recs)))
    {
      /* purecov: begin inspected */
      DBUG_PRINT("error", ("Failed to convert TABLE object to MyISAM "
                           "key and column definition"));
      goto err;
      /* purecov: end */
    }
    if (check_definition(keyinfo, recinfo, table->s->keys, recs,
                         file->s->keyinfo, file->s->rec,
                         file->s->base.keys, file->s->base.fields,
                         true, table))
    {
      /* purecov: begin inspected */
      my_errno= HA_ERR_INCOMPATIBLE_DEFINITION;
      goto err;
      /* purecov: end */
    }
  }

  DBUG_EXECUTE_IF("key",
    Debug_key_myisam::print_keys_myisam(table->in_use,
                                        "ha_myisam::open: ",
                                        table, file->s->keyinfo,
                                        file->s->base.keys);
  );
  
  if (test_if_locked & (HA_OPEN_IGNORE_IF_LOCKED | HA_OPEN_TMP_TABLE))
    (void) mi_extra(file, HA_EXTRA_NO_WAIT_LOCK, 0);

  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);

  /*
    Set data_file_name and index_file_name to point at the symlink value
    if table is symlinked (Ie;  Real name is not same as generated name)
  */
  fn_format(name_buff, file->filename, "", MI_NAME_DEXT,
            MY_APPEND_EXT | MY_UNPACK_FILENAME);
  if (my_is_symlink(name_buff))
  {
    my_readlink(readlink_buf, name_buff, MYF(0));
    data_file_name= strdup_root(&table->mem_root, readlink_buf);
  }
  else
    data_file_name= 0;
  fn_format(name_buff, file->filename, "", MI_NAME_IEXT,
            MY_APPEND_EXT | MY_UNPACK_FILENAME);
  if (my_is_symlink(name_buff))
  {
    my_readlink(readlink_buf, name_buff, MYF(0));
    index_file_name= strdup_root(&table->mem_root, readlink_buf);
  }
  else
    index_file_name= 0;

  if (!(test_if_locked & HA_OPEN_WAIT_IF_LOCKED))
    (void) mi_extra(file, HA_EXTRA_WAIT_LOCK, 0);
  if (!table->s->db_record_offset)
    int_table_flags|=HA_REC_NOT_IN_SEQ;
  if (file->s->options & (HA_OPTION_CHECKSUM | HA_OPTION_COMPRESS_RECORD))
  {
    /*
      Set which type of automatic checksum we have
      The old checksum and new checksum are identical if there is no
      null fields.
      Files with new checksum has the HA_OPTION_NULL_FIELDS bit set.
    */      
    if ((file->s->options & HA_OPTION_NULL_FIELDS) ||
        !file->s->has_null_fields)
      int_table_flags|= HA_HAS_NEW_CHECKSUM;
    if (!(file->s->options & HA_OPTION_NULL_FIELDS))
      int_table_flags|= HA_HAS_OLD_CHECKSUM;
  }

  /*
    For static size rows, tell MariaDB that we will access all bytes
    in the record when writing it.  This signals MariaDB to initialize
    the full row to ensure we don't get any errors from valgrind and
    that all bytes in the row is properly reset.
  */
  if (!(file->s->options &
        (HA_OPTION_PACK_RECORD | HA_OPTION_COMPRESS_RECORD)) &&
      (file->s->has_varchar_fields || file->s->has_null_fields))
    int_table_flags|= HA_RECORD_MUST_BE_CLEAN_ON_WRITE;

  for (i= 0; i < table->s->keys; i++)
  {
    plugin_ref parser= table->key_info[i].parser;
    if (table->key_info[i].flags & HA_USES_PARSER)
      file->s->keyinfo[i].parser=
        (struct st_mysql_ftparser *)plugin_decl(parser)->info;
    table->key_info[i].block_size= file->s->keyinfo[i].block_length;
    table->s->key_info[i].block_size= table->key_info[i].block_size;
  }
  my_errno= 0;

  /* Count statistics of usage for newly open normal files */
  if (file->s->reopen == 1 && ! (test_if_locked & HA_OPEN_TMP_TABLE))
  {
    /* use delay_key_write from .frm, not .MYI */
    file->s->delay_key_write= delay_key_write_options == DELAY_KEY_WRITE_ALL ||
                             (delay_key_write_options == DELAY_KEY_WRITE_ON &&
                       table->s->db_create_options & HA_OPTION_DELAY_KEY_WRITE);
    if (file->s->delay_key_write)
      feature_files_opened_with_delayed_keys++;
  }
  goto end;

 err:
  this->close();
 end:
  /*
    Both recinfo and keydef are allocated by my_multi_malloc(), thus only
    recinfo must be freed.
  */
  if (recinfo)
    my_free(recinfo);
  return my_errno;
}

int ha_myisam::close(void)
{
  MI_INFO *tmp=file;
  if (!tmp)
    return 0;
  file=0;
  return mi_close(tmp);
}

int ha_myisam::write_row(const uchar *buf)
{
  /*
    If we have an auto_increment column and we are writing a changed row
    or a new row, then update the auto_increment value in the record.
  */
  if (table->next_number_field && buf == table->record[0])
  {
    int error;
    if ((error= update_auto_increment()))
      return error;
  }
  return mi_write(file,buf);
}

int ha_myisam::setup_vcols_for_repair(HA_CHECK *param)
{
  DBUG_ASSERT(file->s->base.reclength <= file->s->vreclength);
  if (!table->vfield)
    return 0;

  if (file->s->base.reclength == file->s->vreclength)
  {
    bool indexed_vcols= false;
    ulong new_vreclength= file->s->vreclength;
    for (Field **vf= table->vfield; *vf; vf++)
    {
      if (!(*vf)->stored_in_db())
      {
        uint vf_end= ((*vf)->offset(table->record[0]) +
                      (*vf)->pack_length_in_rec());
        set_if_bigger(new_vreclength, vf_end);
        indexed_vcols|= ((*vf)->flags & PART_KEY_FLAG) != 0;
      }
    }
    if (!indexed_vcols)
      return 0;
    file->s->vreclength= new_vreclength;
    if (!mi_alloc_rec_buff(file, -1, &file->rec_buff))
      return HA_ERR_OUT_OF_MEM;
    bzero(file->rec_buff, mi_get_rec_buff_len(file, file->rec_buff));
  }
  DBUG_ASSERT(file->s->base.reclength < file->s->vreclength ||
              !table->s->stored_fields);
  param->fix_record= compute_vcols;
  table->use_all_columns();
  return 0;
}

int ha_myisam::check(THD* thd, HA_CHECK_OPT* check_opt)
{
  if (!file) return HA_ADMIN_INTERNAL_ERROR;
  int error;
  HA_CHECK *param= thd->alloc<HA_CHECK>(1);
  MYISAM_SHARE* share = file->s;
  const char *old_proc_info=thd->proc_info;

  if (!param)
    return HA_ADMIN_INTERNAL_ERROR;

  thd_proc_info(thd, "Checking table");
  myisamchk_init(param);
  param->thd = thd;
  param->op_name =   "check";
  param->db_name=    table->s->db.str;
  param->table_name= table->alias.c_ptr();
  param->testflag = check_opt->flags | T_CHECK | T_SILENT;
  param->stats_method= (enum_handler_stats_method)THDVAR(thd, stats_method);

  if (!(table->db_stat & HA_READ_ONLY))
    param->testflag|= T_STATISTICS;
  param->using_global_keycache = 1;

  if (!mi_is_crashed(file) &&
      (((param->testflag & T_CHECK_ONLY_CHANGED) &&
	!(share->state.changed & (STATE_CHANGED | STATE_CRASHED |
				  STATE_CRASHED_ON_REPAIR)) &&
	share->state.open_count == 0) ||
       ((param->testflag & T_FAST) && (share->state.open_count ==
				      (uint) (share->global_changed ? 1 : 0)))))
    return HA_ADMIN_ALREADY_DONE;

  if ((error = setup_vcols_for_repair(param)))
  {
    thd_proc_info(thd, old_proc_info);
    return error;
  }

  error = chk_status(param, file);		// Not fatal
  error = chk_size(param, file);
  if (!error)
    error |= chk_del(param, file, param->testflag);
  if (!error)
    error = chk_key(param, file);
  if (!error)
  {
    if ((!(param->testflag & T_QUICK) &&
	 ((share->options &
	   (HA_OPTION_PACK_RECORD | HA_OPTION_COMPRESS_RECORD)) ||
	  (param->testflag & (T_EXTEND | T_MEDIUM)))) ||
	mi_is_crashed(file))
    {
      ulonglong old_testflag= param->testflag;
      param->testflag|=T_MEDIUM;
      if (!(error= init_io_cache(&param->read_cache, file->dfile,
                                 my_default_record_cache_size, READ_CACHE,
                                 share->pack.header_length, 1, MYF(MY_WME))))
      {
        error= chk_data_link(param, file, MY_TEST(param->testflag & T_EXTEND));
        end_io_cache(&param->read_cache);
      }
      param->testflag= old_testflag;
    }
  }
  if (!error)
  {
    if ((share->state.changed & (STATE_CHANGED |
				 STATE_CRASHED_ON_REPAIR |
				 STATE_CRASHED | STATE_NOT_ANALYZED)) ||
	(param->testflag & T_STATISTICS) ||
	mi_is_crashed(file))
    {
      file->update|=HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
      mysql_mutex_lock(&share->intern_lock);
      share->state.changed&= ~(STATE_CHANGED | STATE_CRASHED |
			       STATE_CRASHED_ON_REPAIR);
      if (!(table->db_stat & HA_READ_ONLY))
	error=update_state_info(param,file,UPDATE_TIME | UPDATE_OPEN_COUNT |
				UPDATE_STAT);
      mysql_mutex_unlock(&share->intern_lock);
      info(HA_STATUS_NO_LOCK | HA_STATUS_TIME | HA_STATUS_VARIABLE |
	   HA_STATUS_CONST);
      /*
        Write a 'table is ok' message to error log if table is ok and
        we have written to error log that table was getting checked
      */
      if (!error && !(table->db_stat & HA_READ_ONLY) &&
          !mi_is_crashed(file) && thd->error_printed_to_log &&
          (param->warning_printed || param->error_printed ||
           param->note_printed))
        mi_check_print_info(param, "Table is fixed");
    }
  }
  else if (!mi_is_crashed(file) && !thd->killed)
  {
    mi_mark_crashed(file);
    file->update |= HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
  }

  thd_proc_info(thd, old_proc_info);
  return error ? HA_ADMIN_CORRUPT : HA_ADMIN_OK;
}


/*
  analyze the key distribution in the table
  As the table may be only locked for read, we have to take into account that
  two threads may do an analyze at the same time!
*/

int ha_myisam::analyze(THD *thd, HA_CHECK_OPT* check_opt)
{
  int error=0;
  HA_CHECK *param= thd->alloc<HA_CHECK>(1);
  MYISAM_SHARE* share = file->s;

  if (!param)
    return HA_ADMIN_INTERNAL_ERROR;

  myisamchk_init(param);
  param->thd = thd;
  param->op_name=    "analyze";
  param->db_name=    table->s->db.str;
  param->table_name= table->alias.c_ptr();
  param->testflag= (T_FAST | T_CHECK | T_SILENT | T_STATISTICS |
                   T_DONT_CHECK_CHECKSUM);
  param->using_global_keycache = 1;
  param->stats_method= (enum_handler_stats_method)THDVAR(thd, stats_method);

  if (!(share->state.changed & STATE_NOT_ANALYZED))
    return HA_ADMIN_ALREADY_DONE;

  if ((error = setup_vcols_for_repair(param)))
    return error;

  error = chk_key(param, file);
  if (!error)
  {
    mysql_mutex_lock(&share->intern_lock);
    error=update_state_info(param,file,UPDATE_STAT);
    mysql_mutex_unlock(&share->intern_lock);
  }
  else if (!mi_is_crashed(file) && !thd->killed)
    mi_mark_crashed(file);

  return error ? HA_ADMIN_CORRUPT : HA_ADMIN_OK;
}


int ha_myisam::repair(THD* thd, HA_CHECK_OPT *check_opt)
{
  int error;
  HA_CHECK *param= thd->alloc<HA_CHECK>(1);
  ha_rows start_records;

  if (!file || !param) return HA_ADMIN_INTERNAL_ERROR;

  myisamchk_init(param);
  param->thd = thd;
  param->op_name=  "repair";
  param->testflag= ((check_opt->flags & ~(T_EXTEND)) |
                   T_SILENT | T_FORCE_CREATE | T_CALC_CHECKSUM |
                   (check_opt->flags & T_EXTEND ? T_REP : T_REP_BY_SORT));
  param->tmpfile_createflag= O_RDWR | O_TRUNC;
  param->sort_buffer_length=  THDVAR(thd, sort_buffer_size);
  param->backup_time= check_opt->start_time;
  start_records=file->state->records;

  if ((error = setup_vcols_for_repair(param)))
    return error;

  while ((error=repair(thd,*param,0)) && param->retry_repair)
  {
    param->retry_repair=0;
    if (test_all_bits(param->testflag,
		      (uint) (T_RETRY_WITHOUT_QUICK | T_QUICK)))
    {
      param->testflag&= ~(T_RETRY_WITHOUT_QUICK | T_QUICK);
      /* Ensure we don't loose any rows when retrying without quick */
      param->testflag|= T_SAFE_REPAIR;
      sql_print_information("Retrying repair of: '%s' including modifying data file",
                            table->s->path.str);
      continue;
    }
    param->testflag&= ~T_QUICK;
    if ((param->testflag & (T_REP_BY_SORT | T_REP_PARALLEL)))
    {
      param->testflag= (param->testflag & ~T_REP_ANY) | T_REP;
      sql_print_information("Retrying repair of: '%s' with keycache",
                            table->s->path.str);
      continue;
    }
    break;
  }

  if (!error && start_records != file->state->records &&
      !(check_opt->flags & T_VERY_SILENT))
  {
    char llbuff[22],llbuff2[22];
    sql_print_information("Found %s of %s rows when repairing '%s'",
                          llstr(file->state->records, llbuff),
                          llstr(start_records, llbuff2),
                          table->s->path.str);
  }
  return error;
}

int ha_myisam::optimize(THD* thd, HA_CHECK_OPT *check_opt)
{
  int error;
  HA_CHECK *param= thd->alloc<HA_CHECK>(1);

  if (!file || !param) return HA_ADMIN_INTERNAL_ERROR;

  myisamchk_init(param);
  param->thd = thd;
  param->op_name= "optimize";
  param->testflag= (check_opt->flags | T_SILENT | T_FORCE_CREATE |
                   T_REP_BY_SORT | T_STATISTICS | T_SORT_INDEX);
  param->tmpfile_createflag= O_RDWR | O_TRUNC;
  param->sort_buffer_length=  THDVAR(thd, sort_buffer_size);

  if ((error = setup_vcols_for_repair(param)))
    return error;

  if ((error= repair(thd,*param,1)) && param->retry_repair)
  {
    sql_print_warning("Warning: Optimize table got errno %d on %s.%s, retrying",
                      my_errno, param->db_name, param->table_name);
    param->testflag&= ~T_REP_BY_SORT;
    error= repair(thd,*param,1);
  }

  return error;
}


/*
  Set current_thd() for parallel worker thread
  This is needed to evaluate vcols as we must have current_thd set.
  This will set current_thd in all threads to the same THD, but it's
  safe, because vcols are always evaluated under info->s->intern_lock.

  This is also used temp_file_size_cb_func() to tmp_space_usage by THD.
*/

C_MODE_START
void myisam_setup_thd_for_repair_thread(void *arg)
{
  THD *thd= (THD*) arg;
  DBUG_ASSERT(thd->shared_thd);
  set_current_thd(thd);
}
C_MODE_END


int ha_myisam::repair(THD *thd, HA_CHECK &param, bool do_optimize)
{
  int error=0;
  ulonglong local_testflag= param.testflag;
  bool optimize_done= !do_optimize, statistics_done=0;
  const char *old_proc_info=thd->proc_info;
  char fixed_name[FN_REFLEN];
  MYISAM_SHARE* share = file->s;
  ha_rows rows= file->state->records;
  my_bool locking= 0;
  DBUG_ENTER("ha_myisam::repair");

  param.db_name=    table->s->db.str;
  param.table_name= table->alias.c_ptr();
  param.using_global_keycache = 1;
  param.thd= thd;
  param.tmpdir= &mysql_tmpdir_list;
  param.out_flag= 0;
  share->state.dupp_key= MI_MAX_KEY;
  strmov(fixed_name,file->filename);

  /*
    Don't lock tables if we have used LOCK TABLE or if we come from
    enable_index()
  */
  if (!thd->locked_tables_mode && ! (param.testflag & T_NO_LOCKS))
  {
    locking= 1;
    if (mi_lock_database(file, table->s->tmp_table ? F_EXTRA_LCK : F_WRLCK))
    {
      mi_check_print_error(&param, ER_THD(thd, ER_CANT_LOCK), my_errno);
      DBUG_RETURN(HA_ADMIN_FAILED);
    }
  }

  if (!do_optimize ||
      ((file->state->del || share->state.split != file->state->records) &&
       (!(param.testflag & T_QUICK) ||
	!(share->state.changed & STATE_NOT_OPTIMIZED_KEYS))))
  {
    ulonglong tmp_key_map= ((local_testflag & T_CREATE_MISSING_KEYS) ?
                            mi_get_mask_all_keys_active(share->base.keys) :
                            share->state.key_map);
    ulonglong testflag= param.testflag;
#ifdef HAVE_MMAP
    bool remap= MY_TEST(share->file_map);
    /*
      mi_repair*() functions family use file I/O even if memory
      mapping is available.

      Since mixing mmap I/O and file I/O may cause various artifacts,
      memory mapping must be disabled.
    */
    if (remap)
      mi_munmap_file(file);
#endif
    /*
      The following is to catch errors when my_errno is no set properly
      during repair
    */
    my_errno= 0;
    if (mi_test_if_sort_rep(file,file->state->records,tmp_key_map,0) &&
	(local_testflag & T_REP_BY_SORT))
    {
      local_testflag|= T_STATISTICS;
      param.testflag|= T_STATISTICS;		// We get this for free
      statistics_done=1;
      if (THDVAR(thd, repair_threads)>1)
      {
        /* TODO: respect myisam_repair_threads variable */
        thd_proc_info(thd, "Parallel repair");
        param.testflag|= T_REP_PARALLEL;
        /*
          Ensure that all threads are using the same THD. This is needed
          to get limit of tmp files to work
        */
        param.init_repair_thread= myisam_setup_thd_for_repair_thread;
        param.init_repair_thread_arg= table->in_use;
        /* Mark that multiple threads are using the thd */
        table->in_use->shared_thd= 1;
        error= mi_repair_parallel(&param, file, fixed_name,
                                  MY_TEST(param.testflag & T_QUICK));
        table->in_use->shared_thd= 0;
      }
      else
      {
        thd_proc_info(thd, "Repair by sorting");
        DEBUG_SYNC(thd, "myisam_before_repair_by_sort");
        error = mi_repair_by_sort(&param, file, fixed_name,
                                  MY_TEST(param.testflag & T_QUICK));
      }
      if (error && file->create_unique_index_by_sort && 
          share->state.dupp_key != MAX_KEY)
      {
        my_errno= HA_ERR_FOUND_DUPP_KEY;
        print_keydup_error(table, &table->key_info[share->state.dupp_key],
                           MYF(0));
      }
    }
    else
    {
      thd_proc_info(thd, "Repair with keycache");
      param.testflag &= ~T_REP_BY_SORT;
      error=  mi_repair(&param, file, fixed_name,
                        MY_TEST(param.testflag & T_QUICK));
    }
    param.testflag= testflag | (param.testflag & T_RETRY_WITHOUT_QUICK);
#ifdef HAVE_MMAP
    if (remap)
      mi_dynmap_file(file, file->state->data_file_length);
#endif
    optimize_done=1;
  }
  if (!error)
  {
    if ((local_testflag & T_SORT_INDEX) &&
	(share->state.changed & STATE_NOT_SORTED_PAGES))
    {
      optimize_done=1;
      thd_proc_info(thd, "Sorting index");
      error=mi_sort_index(&param,file,fixed_name);
    }
    if (!error && !statistics_done && (local_testflag & T_STATISTICS))
    {
      if (share->state.changed & STATE_NOT_ANALYZED)
      {
	optimize_done=1;
	thd_proc_info(thd, "Analyzing");
	error = chk_key(&param, file);
      }
      else
	local_testflag&= ~T_STATISTICS;		// Don't update statistics
    }
  }
  thd_proc_info(thd, "Saving state");
  if (!error)
  {
    if ((share->state.changed & STATE_CHANGED) || mi_is_crashed(file))
    {
      share->state.changed&= ~(STATE_CHANGED | STATE_CRASHED |
			       STATE_CRASHED_ON_REPAIR);
      file->update|=HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
    }
    /*
      the following 'if', thought conceptually wrong,
      is a useful optimization nevertheless.
    */
    if (file->state != &file->s->state.state)
      file->s->state.state = *file->state;
    if (file->s->base.auto_key)
      update_auto_increment_key(&param, file, 1);
    if (optimize_done)
    {
      mysql_mutex_lock(&share->intern_lock);
      error = update_state_info(&param, file,
				UPDATE_TIME | UPDATE_OPEN_COUNT |
				(local_testflag &
				 T_STATISTICS ? UPDATE_STAT : 0));
      mysql_mutex_unlock(&share->intern_lock);
    }
    info(HA_STATUS_NO_LOCK | HA_STATUS_TIME | HA_STATUS_VARIABLE |
	 HA_STATUS_CONST);
    if (rows != file->state->records && ! (param.testflag & T_VERY_SILENT))
    {
      char llbuff[22],llbuff2[22];
      mi_check_print_warning(&param,"Number of rows changed from %s to %s",
			     llstr(rows,llbuff),
			     llstr(file->state->records,llbuff2));
    }
  }
  else
  {
    mi_mark_crashed_on_repair(file);
    file->update |= HA_STATE_CHANGED | HA_STATE_ROW_CHANGED;
    update_state_info(&param, file, 0);
  }
  thd_proc_info(thd, old_proc_info);
  if (locking)
    mi_lock_database(file,F_UNLCK);
  DBUG_RETURN(error ? HA_ADMIN_FAILED :
	      !optimize_done ? HA_ADMIN_ALREADY_DONE : HA_ADMIN_OK);
}


/*
  Assign table indexes to a specific key cache.
*/

int ha_myisam::assign_to_keycache(THD* thd, HA_CHECK_OPT *check_opt)
{
  KEY_CACHE *new_key_cache= check_opt->key_cache;
  const char *errmsg= 0;
  char buf[STRING_BUFFER_USUAL_SIZE];
  int error= HA_ADMIN_OK;
  ulonglong map;
  TABLE_LIST *table_list= table->pos_in_table_list;
  DBUG_ENTER("ha_myisam::assign_to_keycache");

  table->keys_in_use_for_query.clear_all();

  if (table_list->process_index_hints(table))
    DBUG_RETURN(HA_ADMIN_FAILED);
  map= ~(ulonglong) 0;
  if (!table->keys_in_use_for_query.is_clear_all())
    /* use all keys if there's no list specified by the user through hints */
    map= table->keys_in_use_for_query.to_ulonglong();

  if ((error= mi_assign_to_key_cache(file, map, new_key_cache)))
  { 
    my_snprintf(buf, sizeof(buf),
		"Failed to flush to index file (errno: %d)", error);
    errmsg= buf;
    error= HA_ADMIN_CORRUPT;
  }

  if (error != HA_ADMIN_OK)
  {
    /* Send error to user */
    HA_CHECK *param= thd->alloc<HA_CHECK>(1);
    if (!param)
      return HA_ADMIN_INTERNAL_ERROR;

    myisamchk_init(param);
    param->thd= thd;
    param->op_name=    "assign_to_keycache";
    param->db_name=    table->s->db.str;
    param->table_name= table->s->table_name.str;
    param->testflag= 0;
    mi_check_print_error(param, "%s", errmsg);
  }
  DBUG_RETURN(error);
}


/*
  Preload pages of the index file for a table into the key cache.
*/

int ha_myisam::preload_keys(THD* thd, HA_CHECK_OPT *check_opt)
{
  int error;
  const char *errmsg;
  ulonglong map;
  TABLE_LIST *table_list= table->pos_in_table_list;
  my_bool ignore_leaves= table_list->ignore_leaves;
  char buf[MYSQL_ERRMSG_SIZE];

  DBUG_ENTER("ha_myisam::preload_keys");

  table->keys_in_use_for_query.clear_all();

  if (table_list->process_index_hints(table))
    DBUG_RETURN(HA_ADMIN_FAILED);

  map= ~(ulonglong) 0;
  /* Check validity of the index references */
  if (!table->keys_in_use_for_query.is_clear_all())
    /* use all keys if there's no list specified by the user through hints */
    map= table->keys_in_use_for_query.to_ulonglong();

  mi_extra(file, HA_EXTRA_PRELOAD_BUFFER_SIZE,
           (void *) &thd->variables.preload_buff_size);

  if ((error= mi_preload(file, map, ignore_leaves)))
  {
    switch (error) {
    case HA_ERR_NON_UNIQUE_BLOCK_SIZE:
      errmsg= "Indexes use different block sizes";
      break;
    case HA_ERR_OUT_OF_MEM:
      errmsg= "Failed to allocate buffer";
      break;
    default:
      my_snprintf(buf, sizeof(buf),
                  "Failed to read from index file (errno: %d)", my_errno);
      errmsg= buf;
    }
    error= HA_ADMIN_FAILED;
    goto err;
  }

  DBUG_RETURN(HA_ADMIN_OK);

 err:
  {
    HA_CHECK *param= thd->alloc<HA_CHECK>(1);
    if (!param)
      return HA_ADMIN_INTERNAL_ERROR;
    myisamchk_init(param);
    param->thd= thd;
    param->op_name=    "preload_keys";
    param->db_name=    table->s->db.str;
    param->table_name= table->s->table_name.str;
    param->testflag=   0;
    mi_check_print_error(param, "%s", errmsg);
    DBUG_RETURN(error);
  }
}


/*
  Disable indexes, making it persistent if requested.

  SYNOPSIS
    disable_indexes()

  DESCRIPTION
    See handler::ha_disable_indexes()

  RETURN
    0  ok
    HA_ERR_WRONG_COMMAND  mode not implemented.
*/

int ha_myisam::disable_indexes(key_map map, bool persist)
{
  int error;

  if (!persist)
  {
    /* call a storage engine function to switch the key map */
    DBUG_ASSERT(map.is_clear_all());
    error= mi_disable_indexes(file);
  }
  else
  {
    ulonglong ullmap= map.to_ulonglong();

    /* make sure auto-inc key is enabled even if it's > 64 */
    if (map.length() > MI_KEYMAP_BITS &&
        table->s->next_number_index < MAX_KEY)
      mi_set_key_active(ullmap, table->s->next_number_index);

    mi_extra(file, HA_EXTRA_NO_KEYS, &ullmap);
    info(HA_STATUS_CONST);                        // Read new key info
    error= 0;
  }
  return error;
}


/*
  Enable indexes, making it persistent if requested.

  SYNOPSIS
    enable_indexes()

  DESCRIPTION
    Enable indexes, which might have been disabled by disable_index() before.
    If persist=false, it works only if both data and indexes are empty,
    since the MyISAM repair would enable them persistently.
    To be sure in these cases, call handler::delete_all_rows() before.

    See also handler::ha_enable_indexes()

  RETURN
    0  ok
    !=0  Error, among others:
    HA_ERR_CRASHED  data or index is non-empty. Delete all rows and retry.
    HA_ERR_WRONG_COMMAND  mode not implemented.
*/

int ha_myisam::enable_indexes(key_map map, bool persist)
{
  int error;
  DBUG_ENTER("ha_myisam::enable_indexes");

  DBUG_EXECUTE_IF("wait_in_enable_indexes",
                  debug_wait_for_kill("wait_in_enable_indexes"); );

  if (mi_is_all_keys_active(file->s->state.key_map, file->s->base.keys))
  {
    /* All indexes are enabled already. */
    DBUG_RETURN(0);
  }

  DBUG_ASSERT(map.is_prefix(table->s->keys));
  if (!persist)
  {
    error= mi_enable_indexes(file);
    /*
       Do not try to repair on error,
       as this could make the enabled state persistent,
       but mode==HA_KEY_SWITCH_ALL forbids it.
    */
  }
  else
  {
    THD *thd= table->in_use;
    int was_error= thd->is_error();
    HA_CHECK *param= thd->alloc<HA_CHECK>(1);
    const char *save_proc_info=thd->proc_info;

    if (!param)
      DBUG_RETURN(HA_ADMIN_INTERNAL_ERROR);

    thd_proc_info(thd, "Creating index");
    myisamchk_init(param);
    param->op_name= "recreating_index";
    param->testflag= (T_SILENT | T_REP_BY_SORT | T_QUICK |
                     T_CREATE_MISSING_KEYS);
    /*
      Don't lock and unlock table if it's locked.
      Normally table should be locked.  This test is mostly for safety.
    */
    if (likely(file->lock_type != F_UNLCK))
      param->testflag|= T_NO_LOCKS;

    if (file->create_unique_index_by_sort)
      param->testflag|= T_CREATE_UNIQUE_BY_SORT;

    param->myf_rw&= ~MY_WAIT_IF_FULL;
    param->sort_buffer_length=  THDVAR(thd, sort_buffer_size);
    param->stats_method= (enum_handler_stats_method)THDVAR(thd, stats_method);
    param->tmpdir=&mysql_tmpdir_list;

    if ((error = setup_vcols_for_repair(param)))
    {
      thd_proc_info(thd, save_proc_info);
      DBUG_RETURN(error);
    }

    if ((error= (repair(thd,*param,0) != HA_ADMIN_OK)) && param->retry_repair)
    {
      sql_print_warning("Warning: Enabling keys got errno %d on %s.%s, retrying",
                        my_errno, param->db_name, param->table_name);
      /*
        Repairing by sort failed. Now try standard repair method.
        Still we want to fix only index file. If data file corruption
        was detected (T_RETRY_WITHOUT_QUICK), we shouldn't do much here.
        Let implicit repair do this job.
      */
      if (!(param->testflag & T_RETRY_WITHOUT_QUICK))
      {
        param->testflag&= ~T_REP_BY_SORT;
        error= (repair(thd,*param,0) != HA_ADMIN_OK);
      }
      /*
        If the standard repair succeeded, clear all error messages which
        might have been set by the first repair. They can still be seen
        with SHOW WARNINGS then.
      */
      if (! error && ! was_error)
        thd->clear_error();
    }
    info(HA_STATUS_CONST);
    thd_proc_info(thd, save_proc_info);
  }
  DBUG_RETURN(error);
}


/*
  Test if indexes are disabled.


  SYNOPSIS
    indexes_are_disabled()
      no parameters


  RETURN
    0  indexes are not disabled
    1  all indexes are disabled
   [2  non-unique indexes are disabled - NOT YET IMPLEMENTED]
*/

int ha_myisam::indexes_are_disabled(void)
{
  
  return mi_indexes_are_disabled(file);
}


/*
  prepare for a many-rows insert operation
  e.g. - disable indexes (if they can be recreated fast) or
  activate special bulk-insert optimizations

  SYNOPSIS
    start_bulk_insert(rows, flags)
    rows        Rows to be inserted
                0 if we don't know
    flags       Flags to control index creation

  NOTICE
    Do not forget to call end_bulk_insert() later!
*/

void ha_myisam::start_bulk_insert(ha_rows rows, uint flags)
{
  DBUG_ENTER("ha_myisam::start_bulk_insert");
  THD *thd= table->in_use;
  ulong size= MY_MIN(thd->variables.read_buff_size,
                     (ulong) (table->s->avg_row_length*rows));
  bool index_disabled= 0;
  DBUG_PRINT("info",("start_bulk_insert: rows %lu size %lu",
                     (ulong) rows, size));

  /* don't enable row cache if too few rows */
  if ((!rows || rows > MI_MIN_ROWS_TO_USE_WRITE_CACHE) && !has_long_unique())
    mi_extra(file, HA_EXTRA_WRITE_CACHE, (void*) &size);

  can_enable_indexes= mi_is_all_keys_active(file->s->state.key_map,
                                            file->s->base.keys);

  /*
    Only disable old index if the table was empty and we are inserting
    a lot of rows.
    Note that in end_bulk_insert() we may truncate the table if
    enable_indexes() failed, thus it's essential that indexes are
    disabled ONLY for an empty table.
  */
  if (file->state->records == 0 && can_enable_indexes &&
      (!rows || rows >= MI_MIN_ROWS_TO_DISABLE_INDEXES))
  {
    if (file->open_flag & HA_OPEN_INTERNAL_TABLE)
    {
      file->update|= HA_STATE_CHANGED;
      index_disabled= file->s->base.keys > 0;
      mi_clear_all_keys_active(file->s->state.key_map);
    }
    else
    {
      my_bool all_keys= MY_TEST(flags & HA_CREATE_UNIQUE_INDEX_BY_SORT);
      MYISAM_SHARE *share=file->s;
      MI_KEYDEF    *key=share->keyinfo;
      uint          i;
     /*
      Deactivate all indexes that can be recreated fast.
      These include packed keys on which sorting will use more temporary
      space than the max allowed file length or for which the unpacked keys
      will take much more space than packed keys.
      Note that 'rows' may be zero for the case when we don't know how many
      rows we will put into the file.
      Long Unique Index (HA_KEY_ALG_LONG_HASH) will not be disabled because
      there unique property is enforced at the time of ha_write_row
      (check_duplicate_long_entries). So we need active index at the time of
      insert.
     */
      DBUG_ASSERT(file->state->records == 0 &&
                  (!rows || rows >= MI_MIN_ROWS_TO_DISABLE_INDEXES));
      for (i=0 ; i < share->base.keys ; i++,key++)
      {
        if (!(key->flag & HA_AUTO_KEY) && file->s->base.auto_key != i+1 &&
            ! mi_too_big_key_for_sort(key,rows) &&
            (all_keys || !(key->flag & HA_NOSAME)) &&
            table->key_info[i].algorithm != HA_KEY_ALG_LONG_HASH &&
            table->key_info[i].algorithm != HA_KEY_ALG_RTREE)
        {
          mi_clear_key_active(share->state.key_map, i);
          index_disabled= 1;
          file->update|= HA_STATE_CHANGED;
          file->create_unique_index_by_sort= all_keys;
        }
      }
    }
  }
  else
  {
    if (!file->bulk_insert &&
        (!rows || rows >= MI_MIN_ROWS_TO_USE_BULK_INSERT))
    {
      mi_init_bulk_insert(file, (size_t) thd->variables.bulk_insert_buff_size,
                          rows);
    }
  }
  can_enable_indexes= index_disabled;
  DBUG_VOID_RETURN;
}

/*
  end special bulk-insert optimizations,
  which have been activated by start_bulk_insert().

  SYNOPSIS
    end_bulk_insert(fatal_error)
    abort         0 normal end, store everything
                  1 abort quickly. No need to flush/write anything. Table will be deleted

  RETURN
    0     OK
    != 0  Error
*/

int ha_myisam::end_bulk_insert()
{
  int first_error, error;
  my_bool abort= file->s->deleting;
  DBUG_ENTER("ha_myisam::end_bulk_insert");

  if ((first_error= mi_end_bulk_insert(file, abort)))
    abort= 1;

  if ((error= mi_extra(file, HA_EXTRA_NO_CACHE, 0)))
  {
    first_error= first_error ? first_error : error;
    abort= 1;
  }

  if (!abort)
  {
    if (can_enable_indexes)
    {
      /* 
        Truncate the table when enable index operation is killed. 
        After truncating the table we don't need to enable the 
        indexes, because the last repair operation is aborted after 
        setting the indexes as active and  trying to recreate them. 
     */
   
      if (((first_error= enable_indexes(key_map(table->s->keys), true))) &&
          table->in_use->killed)
      {
        delete_all_rows();
        /* not crashed, despite being killed during repair */
        file->s->state.changed&= ~(STATE_CRASHED|STATE_CRASHED_ON_REPAIR);
      }
    }
    can_enable_indexes= 0;
  }
  DBUG_PRINT("exit", ("first_error: %d", first_error));
  DBUG_RETURN(first_error);
}


bool ha_myisam::check_and_repair(THD *thd)
{
  int error=0;
  int marked_crashed;
  HA_CHECK_OPT check_opt;
  DBUG_ENTER("ha_myisam::check_and_repair");

  check_opt.init();
  check_opt.flags= T_MEDIUM | T_AUTO_REPAIR;
  // Don't use quick if deleted rows
  if (!file->state->del && (myisam_recover_options & HA_RECOVER_QUICK))
    check_opt.flags|=T_QUICK;
  sql_print_warning("Checking table:   '%s'",table->s->path.str);

  const CSET_STRING query_backup= thd->query_string;
  thd->set_query((char*) table->s->table_name.str,
                 (uint) table->s->table_name.length, system_charset_info);

  if ((marked_crashed= mi_is_crashed(file)) || check(thd, &check_opt))
  {
    bool save_log_all_errors;
    sql_print_warning("Recovering table: '%s'",table->s->path.str);
    save_log_all_errors= thd->log_all_errors;
    thd->log_all_errors|= (thd->variables.log_warnings > 2);
    if (myisam_recover_options & HA_RECOVER_FULL_BACKUP)
    {
      char buff[MY_BACKUP_NAME_EXTRA_LENGTH+1];
      my_create_backup_name(buff, "", check_opt.start_time);
      sql_print_information("Making backup of index file %s with extension '%s'",
                            file->s->index_file_name, buff);
      mi_make_backup_of_index(file, check_opt.start_time,
                              MYF(MY_WME | ME_WARNING));
    }
    check_opt.flags=
      (((myisam_recover_options &
         (HA_RECOVER_BACKUP | HA_RECOVER_FULL_BACKUP)) ? T_BACKUP_DATA : 0) |
       (marked_crashed                             ? 0 : T_QUICK) |
       (myisam_recover_options & HA_RECOVER_FORCE  ? 0 : T_SAFE_REPAIR) |
       T_AUTO_REPAIR);
    if (repair(thd, &check_opt))
      error=1;
    thd->log_all_errors= save_log_all_errors;
  }
  thd->set_query(query_backup);
  DBUG_RETURN(error);
}

bool ha_myisam::is_crashed() const
{
  return (file->s->state.changed & STATE_CRASHED ||
	  (my_disable_locking && file->s->state.open_count));
}

int ha_myisam::update_row(const uchar *old_data, const uchar *new_data)
{
  return mi_update(file,old_data,new_data);
}

int ha_myisam::delete_row(const uchar *buf)
{
  return mi_delete(file,buf);
}


int ha_myisam::index_init(uint idx, bool sorted)
{ 
  active_index=idx;
  if (pushed_idx_cond_keyno == idx)
    mi_set_index_cond_func(file, handler_index_cond_check, this);
  if (pushed_rowid_filter && handler_rowid_filter_is_active(this))
    mi_set_rowid_filter_func(file, handler_rowid_filter_check, this);
  return 0; 
}


int ha_myisam::index_end()
{
  DBUG_ENTER("ha_myisam::index_end");
  active_index= MAX_KEY;
  mi_set_index_cond_func(file, NULL, 0);
  in_range_check_pushed_down= FALSE;
  mi_set_rowid_filter_func(file, NULL, 0);
  ds_mrr.dsmrr_close();
#if !defined(DBUG_OFF) && defined(SQL_SELECT_FIXED_FOR_UPDATE)
  file->update&= ~HA_STATE_AKTIV;               // Forget active row
#endif
  DBUG_RETURN(0);
}

int ha_myisam::rnd_end()
{
  DBUG_ENTER("ha_myisam::rnd_end");
  ds_mrr.dsmrr_close();
#if !defined(DBUG_OFF) && defined(SQL_SELECT_FIXED_FOR_UPDATE)
  file->update&= ~HA_STATE_AKTIV;               // Forget active row
#endif
  DBUG_RETURN(0);
}

int ha_myisam::index_read_map(uchar *buf, const uchar *key,
                              key_part_map keypart_map,
                              enum ha_rkey_function find_flag)
{
  DBUG_ASSERT(inited==INDEX);
  int error=mi_rkey(file, buf, active_index, key, keypart_map, find_flag);
  return error;
}

int ha_myisam::index_read_idx_map(uchar *buf, uint index, const uchar *key,
                                  key_part_map keypart_map,
                                  enum ha_rkey_function find_flag)
{
  int res;
  /* Use the pushed index condition if it matches the index we're scanning */
  end_range= NULL;
  if (index == pushed_idx_cond_keyno)
    mi_set_index_cond_func(file, handler_index_cond_check, this);
  if (pushed_rowid_filter && handler_rowid_filter_is_active(this))
    mi_set_rowid_filter_func(file, handler_rowid_filter_check, this);
  res= mi_rkey(file, buf, index, key, keypart_map, find_flag);
  mi_set_index_cond_func(file, NULL, 0);
  return res;
}

int ha_myisam::index_next(uchar *buf)
{
  DBUG_ASSERT(inited==INDEX);
  int error=mi_rnext(file,buf,active_index);
  return error;
}

int ha_myisam::index_prev(uchar *buf)
{
  DBUG_ASSERT(inited==INDEX);
  int error=mi_rprev(file,buf, active_index);
  return error;
}

int ha_myisam::index_first(uchar *buf)
{
  DBUG_ASSERT(inited==INDEX);
  int error=mi_rfirst(file, buf, active_index);
  return error;
}

int ha_myisam::index_last(uchar *buf)
{
  DBUG_ASSERT(inited==INDEX);
  int error=mi_rlast(file, buf, active_index);
  return error;
}

int ha_myisam::index_next_same(uchar *buf,
			       const uchar *key __attribute__((unused)),
			       uint length __attribute__((unused)))
{
  int error;
  DBUG_ASSERT(inited==INDEX);
  do
  {
    error= mi_rnext_same(file,buf);
  } while (error == HA_ERR_RECORD_DELETED);
  return error;
}


int ha_myisam::rnd_init(bool scan)
{
  if (scan)
    return mi_scan_init(file);
  return mi_reset(file);                        // Free buffers
}

int ha_myisam::rnd_next(uchar *buf)
{
  int error=mi_scan(file, buf);
  return error;
}

int ha_myisam::remember_rnd_pos()
{
  position((uchar*) 0);
  return 0;
}

int ha_myisam::restart_rnd_next(uchar *buf)
{
  return rnd_pos(buf, ref);
}

int ha_myisam::rnd_pos(uchar *buf, uchar *pos)
{
  int error=mi_rrnd(file, buf, my_get_ptr(pos,ref_length));
  return error;
}

void ha_myisam::position(const uchar *record)
{
  my_off_t row_position= mi_position(file);
  my_store_ptr(ref, ref_length, row_position);
  file->update|= HA_STATE_AKTIV;               // Row can be updated
}

int ha_myisam::info(uint flag)
{
  MI_ISAMINFO misam_info;

  if (!table)
    return 1;

  (void) mi_status(file,&misam_info,flag);
  if (flag & HA_STATUS_VARIABLE)
  {
    stats.records=           misam_info.records;
    stats.deleted=           misam_info.deleted;
    stats.data_file_length=  misam_info.data_file_length;
    stats.index_file_length= misam_info.index_file_length;
    stats.delete_length=     misam_info.delete_length;
    stats.check_time=        (ulong) misam_info.check_time;
    stats.mean_rec_length=   misam_info.mean_reclength;
    stats.checksum=          file->state->checksum;
  }
  if (flag & HA_STATUS_CONST)
  {
    TABLE_SHARE *share= table->s;
    stats.max_data_file_length=  misam_info.max_data_file_length;
    stats.max_index_file_length= misam_info.max_index_file_length;
    stats.create_time= (ulong) misam_info.create_time;
    /* 
      We want the value of stats.mrr_length_per_rec to be platform independent.
      The size of the chunk at the end of the join buffer used for MRR needs
      is calculated now basing on the values passed in the stats structure.
      The remaining part of the join buffer is used for records. A different
      number of records in the buffer results in a different number of buffer
      refills and in a different order of records in the result set.
    */
    stats.mrr_length_per_rec= misam_info.reflength + 8; // 8=MY_MAX(sizeof(void *))

    ref_length= misam_info.reflength;
    share->db_options_in_use= misam_info.options;
    /* record block size. We adjust with IO_SIZE to not make it too small */
    stats.block_size= MY_MAX(myisam_block_size, IO_SIZE);

    if (table_share->tmp_table == NO_TMP_TABLE)
      mysql_mutex_lock(&table_share->LOCK_share);
    share->keys_in_use.set_prefix(share->keys);
    share->keys_in_use.intersect_extended(misam_info.key_map);
    share->keys_for_keyread.intersect(share->keys_in_use);
    share->db_record_offset= misam_info.record_offset;
    if (share->key_parts)
    {
      ulong *from= misam_info.rec_per_key;
      KEY *key, *key_end;
      for (key= table->key_info, key_end= key + share->keys;
           key < key_end ; key++)
      {
        memcpy(key->rec_per_key, from,
               key->user_defined_key_parts * sizeof(*from));
        from+= key->user_defined_key_parts;
      }
    }
    if (table_share->tmp_table == NO_TMP_TABLE)
      mysql_mutex_unlock(&table_share->LOCK_share);
  }
  if (flag & HA_STATUS_ERRKEY)
  {
    errkey  = misam_info.errkey;
    my_store_ptr(dup_ref, ref_length, misam_info.dupp_key_pos);
  }
  if (flag & HA_STATUS_TIME)
    stats.update_time = (ulong) misam_info.update_time;
  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value= misam_info.auto_increment;

  return 0;
}


int ha_myisam::extra(enum ha_extra_function operation)
{
  if ((operation == HA_EXTRA_MMAP && !opt_myisam_use_mmap) ||
      (operation == HA_EXTRA_WRITE_CACHE && has_long_unique()))
    return 0;
  return mi_extra(file, operation, 0);
}


int ha_myisam::reset(void)
{
  mi_set_index_cond_func(file, NULL, 0);
  ds_mrr.dsmrr_close();
  return mi_reset(file);
}

/* To be used with WRITE_CACHE and EXTRA_CACHE */

int ha_myisam::extra_opt(enum ha_extra_function operation, ulong cache_size)
{
  return mi_extra(file, operation, (void*) &cache_size);
}

int ha_myisam::delete_all_rows()
{
  return mi_delete_all_rows(file);
}


int ha_myisam::reset_auto_increment(ulonglong value)
{
  file->s->state.auto_increment= value;
  return 0;
}

int ha_myisam::delete_table(const char *name)
{
  return mi_delete_table(name);
}

void ha_myisam::change_table_ptr(TABLE *table_arg, TABLE_SHARE *share)
{
  handler::change_table_ptr(table_arg, share);
  if (file)
    file->external_ref= table_arg;
}


int ha_myisam::external_lock(THD *thd, int lock_type)
{
  file->in_use.data= thd;
  file->external_ref= (void*) table;            // For mi_killed()
  return mi_lock_database(file, !table->s->tmp_table ?
			  lock_type : ((lock_type == F_UNLCK) ?
				       F_UNLCK : F_EXTRA_LCK));
}

THR_LOCK_DATA **ha_myisam::store_lock(THD *thd,
				      THR_LOCK_DATA **to,
				      enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && file->lock.type == TL_UNLOCK)
    file->lock.type=lock_type;
  *to++= &file->lock;
  return to;
}

void ha_myisam::update_create_info(HA_CREATE_INFO *create_info)
{
  ha_myisam::info(HA_STATUS_AUTO | HA_STATUS_CONST);
  if (!(create_info->used_fields & HA_CREATE_USED_AUTO))
  {
    create_info->auto_increment_value= stats.auto_increment_value;
  }
  create_info->data_file_name=data_file_name;
  create_info->index_file_name=index_file_name;
}


int ha_myisam::create(const char *name, TABLE *table_arg,
		      HA_CREATE_INFO *ha_create_info)
{
  int error;
  uint create_flags= 0, record_count, i;
  char buff[FN_REFLEN];
  MI_KEYDEF *keydef;
  MI_COLUMNDEF *recinfo;
  MI_CREATE_INFO create_info;
  TABLE_SHARE *share= table_arg->s;
  uint options= share->db_options_in_use;
  DBUG_ENTER("ha_myisam::create");

  for (i= 0; i < share->virtual_fields && !create_flags; i++)
    if (table_arg->vfield[i]->flags & PART_KEY_FLAG)
      create_flags|= HA_CREATE_RELIES_ON_SQL_LAYER;
  for (i= 0; i < share->keys && !create_flags; i++)
    if (table_arg->key_info[i].flags & HA_USES_PARSER)
      create_flags|= HA_CREATE_RELIES_ON_SQL_LAYER;

  if ((error= table2myisam(table_arg, &keydef, &recinfo, &record_count)))
    DBUG_RETURN(error); /* purecov: inspected */

#ifndef DBUG_OFF
  DBUG_EXECUTE_IF("key",
    Debug_key_myisam::print_keys_myisam(table_arg->in_use,
                                        "ha_myisam::create: ",
                                        table_arg, keydef, share->keys);
  );
#endif

  bzero((char*) &create_info, sizeof(create_info));
  create_info.max_rows= share->max_rows;
  create_info.reloc_rows= share->min_rows;
  create_info.with_auto_increment= share->next_number_key_offset == 0;
  create_info.auto_increment= (ha_create_info->auto_increment_value ?
                               ha_create_info->auto_increment_value -1 :
                               (ulonglong) 0);
  create_info.data_file_length= ((ulonglong) share->max_rows *
                                 share->avg_row_length);
  create_info.language= share->table_charset->number;

#ifdef HAVE_READLINK
  if (my_use_symdir)
  {
    create_info.data_file_name= ha_create_info->data_file_name;
    create_info.index_file_name= ha_create_info->index_file_name;
  }
  else
#endif /* HAVE_READLINK */
  {
    THD *thd= table_arg->in_use;
    if (ha_create_info->data_file_name)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          WARN_OPTION_IGNORED,
                          ER_THD(thd, WARN_OPTION_IGNORED),
                          "DATA DIRECTORY");
    if (ha_create_info->index_file_name)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          WARN_OPTION_IGNORED,
                          ER_THD(thd, WARN_OPTION_IGNORED),
                          "INDEX DIRECTORY");
  }

  if (ha_create_info->tmp_table())
    create_flags|= HA_CREATE_TMP_TABLE | HA_CREATE_DELAY_KEY_WRITE;
  if (ha_create_info->options & HA_CREATE_KEEP_FILES)
    create_flags|= HA_CREATE_KEEP_FILES;
  if (options & HA_OPTION_PACK_RECORD)
    create_flags|= HA_PACK_RECORD;
  if (options & HA_OPTION_CHECKSUM)
    create_flags|= HA_CREATE_CHECKSUM;
  if (options & HA_OPTION_DELAY_KEY_WRITE)
    create_flags|= HA_CREATE_DELAY_KEY_WRITE;

  /* TODO: Check that the following fn_format is really needed */
  error= mi_create(fn_format(buff, name, "", "",
                             MY_UNPACK_FILENAME|MY_APPEND_EXT),
                   share->keys, keydef,
                   record_count, recinfo,
                   0, (MI_UNIQUEDEF*) 0,
                   &create_info, create_flags);
  ref_length= create_info.rec_reflength;
  my_free(recinfo);
  DBUG_RETURN(error);
}


int ha_myisam::rename_table(const char * from, const char * to)
{
  return mi_rename(from,to);
}


void ha_myisam::get_auto_increment(ulonglong offset, ulonglong increment,
                                   ulonglong nb_desired_values,
                                   ulonglong *first_value,
                                   ulonglong *nb_reserved_values)
{
  ulonglong nr;
  int error;
  uchar key[HA_MAX_KEY_LENGTH];
  enum ha_rkey_function search_flag= HA_READ_PREFIX_LAST;

  if (!table->s->next_number_key_offset)
  {						// Autoincrement at key-start
    ha_myisam::info(HA_STATUS_AUTO);
    *first_value= stats.auto_increment_value;
    /* MyISAM has only table-level lock, so reserves to +inf */
    *nb_reserved_values= ULONGLONG_MAX;
    return;
  }

  /* it's safe to call the following if bulk_insert isn't on */
  mi_flush_bulk_insert(file, table->s->next_number_index);

  if (unlikely(table->key_info[table->s->next_number_index].
                  key_part[table->s->next_number_keypart].key_part_flag &
                    HA_REVERSE_SORT))
    search_flag= HA_READ_KEY_EXACT;

  (void) extra(HA_EXTRA_KEYREAD);
  key_copy(key, table->record[0],
           table->key_info + table->s->next_number_index,
           table->s->next_number_key_offset);
  error= mi_rkey(file, table->record[1], (int) table->s->next_number_index,
                 key, make_prev_keypart_map(table->s->next_number_keypart),
                 search_flag);
  if (error)
    nr= 1;
  else
  {
    /* Get data from record[1] */
    nr= ((ulonglong) table->next_number_field->
         val_int_offset(table->s->rec_buff_length)+1);
  }
  extra(HA_EXTRA_NO_KEYREAD);
  *first_value= nr;
  /*
    MySQL needs to call us for next row: assume we are inserting ("a",null)
    here, we return 3, and next this statement will want to insert ("b",null):
    there is no reason why ("b",3+1) would be the good row to insert: maybe it
    already exists, maybe 3+1 is too large...
  */
  *nb_reserved_values= 1;
}


/*
  Find out how many rows there is in the given range

  SYNOPSIS
    records_in_range()
    inx			Index to use
    min_key		Start of range.  Null pointer if from first key
    max_key		End of range. Null pointer if to last key
    pages               Store first and last page for the range in case of
                        b-trees. In other cases it's not touched.

  NOTES
    min_key.flag can have one of the following values:
      HA_READ_KEY_EXACT		Include the key in the range
      HA_READ_AFTER_KEY		Don't include key in range

    max_key.flag can have one of the following values:  
      HA_READ_BEFORE_KEY	Don't include key in range
      HA_READ_AFTER_KEY		Include all 'end_key' values in the range

  RETURN
   HA_POS_ERROR		Something is wrong with the index tree.
   0			There is no matching keys in the given range
   number > 0		There is approximately 'number' matching rows in
			the range.
*/

ha_rows ha_myisam::records_in_range(uint inx, const key_range *min_key,
                                    const key_range *max_key,
                                    page_range *pages)
{
  return (ha_rows) mi_records_in_range(file, (int) inx, min_key, max_key,
                                       pages);
}


int ha_myisam::ft_read(uchar *buf)
{
  int error;

  if (!ft_handler)
    return -1;

  thread_safe_increment(table->in_use->status_var.ha_read_next_count,
			&LOCK_status); // why ?

  error=ft_handler->please->read_next(ft_handler,(char*) buf);
  return error;
}

enum_alter_inplace_result
ha_myisam::check_if_supported_inplace_alter(TABLE *new_table,
                                            Alter_inplace_info *alter_info)
{
  DBUG_ENTER("ha_myisam::check_if_supported_inplace_alter");

  const alter_table_operations readd_index=
                          ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX |
                          ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX;
  const alter_table_operations readd_unique=
                          ALTER_ADD_UNIQUE_INDEX |
                          ALTER_DROP_UNIQUE_INDEX;
  const alter_table_operations readd_pk=
                          ALTER_ADD_PK_INDEX |
                          ALTER_DROP_PK_INDEX;

  const  alter_table_operations op= alter_info->handler_flags;

  if (op & ALTER_COLUMN_VCOL)
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  /*
    ha_myisam::open() updates table->key_info->block_size to be the actual
    MYI index block size, overwriting user-specified value (if any).
    So, the server can not reliably detect whether ALTER TABLE changes
    key_block_size or not, it might think the block size was changed,
    when it wasn't, and in this case the server will recreate (drop+add)
    the index unnecessary. Fix it.
  */

  if (table->s->keys == new_table->s->keys &&
      ((op & readd_pk) == readd_pk ||
       (op & readd_unique) == readd_unique ||
       (op & readd_index) == readd_index))
  {
    for (uint i=0; i < table->s->keys; i++)
    {
      KEY *old_key= table->key_info + i;
      KEY *new_key= new_table->key_info + i;

      if (old_key->block_size == new_key->block_size)
        DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED); // must differ somewhere else

      if (new_key->block_size && new_key->block_size != old_key->block_size)
        DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED); // really changed

      /* any difference besides the block_size, and we give up */
      if (old_key->key_length != new_key->key_length ||
          old_key->flags != new_key->flags ||
          old_key->user_defined_key_parts != new_key->user_defined_key_parts ||
          old_key->algorithm != new_key->algorithm ||
          strcmp(old_key->name.str, new_key->name.str))
        DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

      for (uint j= 0; j < old_key->user_defined_key_parts; j++)
      {
        KEY_PART_INFO *old_kp= old_key->key_part + j;
        KEY_PART_INFO *new_kp= new_key->key_part + j;
        if (old_kp->offset != new_kp->offset ||
            old_kp->null_offset != new_kp->null_offset ||
            old_kp->length != new_kp->length ||
            old_kp->fieldnr != new_kp->fieldnr ||
            old_kp->key_part_flag != new_kp->key_part_flag ||
            old_kp->type != new_kp->type ||
            old_kp->null_bit != new_kp->null_bit)
          DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
      }
    }
    alter_info->handler_flags &= ~(readd_pk | readd_unique | readd_index);
  }
  DBUG_RETURN(handler::check_if_supported_inplace_alter(new_table, alter_info));
}


static bool directories_differ(const char *d1, const char *d2)
{
  if (!d1 && !d2)
    return false;
  if (!d1 || !d2)
    return true;
  size_t l1= dirname_length(d1), l2= dirname_length(d2);
  return l1 != l2 || strncmp(d1, d2, l1);
}


bool ha_myisam::check_if_incompatible_data(HA_CREATE_INFO *create_info,
					   uint table_changes)
{
  uint options= table->s->db_options_in_use;

  if ((create_info->used_fields & HA_CREATE_USED_AUTO &&
       create_info->auto_increment_value != stats.auto_increment_value) ||
      directories_differ(create_info->data_file_name, data_file_name) ||
      directories_differ(create_info->index_file_name, index_file_name) ||
      table_changes == IS_EQUAL_NO ||
      table_changes & IS_EQUAL_PACK_LENGTH) // Not implemented yet
    return COMPATIBLE_DATA_NO;

  if ((options & (HA_OPTION_PACK_RECORD | HA_OPTION_CHECKSUM)) !=
      (create_info->table_options & (HA_OPTION_PACK_RECORD | HA_OPTION_CHECKSUM)))
    return COMPATIBLE_DATA_NO;
  return COMPATIBLE_DATA_YES;
}


/**
  Check if a table is incompatible with the current version.

  The cases are:
  - Table has checksum, varchars and are not of dynamic record type
*/

int ha_myisam::check_for_upgrade(HA_CHECK_OPT *check_opt)
{
  if ((file->s->options & HA_OPTION_CHECKSUM) &&
      !(file->s->options & HA_OPTION_NULL_FIELDS) &&
      !(file->s->options & HA_OPTION_PACK_RECORD) &&
      file->s->has_varchar_fields)
  {
    /* We need alter there to get the HA_OPTION_NULL_FIELDS flag to be set */
    return HA_ADMIN_NEEDS_ALTER;
  }
  return HA_ADMIN_OK;
}


extern int mi_panic(enum ha_panic_function flag);
int myisam_panic(handlerton *hton, ha_panic_function flag)
{
  return mi_panic(flag);
}

static int myisam_drop_table(handlerton *hton, const char *path)
{
  return mi_delete_table(path);
}


void myisam_update_optimizer_costs(OPTIMIZER_COSTS *costs)
{
  /*
    MyISAM row lookup costs are slow as the row data is not cached
    The following numbers where found by check_costs.pl when using 1M rows
    and all rows are cached. See optimizer_costs.txt
  */
  costs->row_next_find_cost=   0.000063539;
  costs->row_lookup_cost=      0.001014818;
  costs->key_next_find_cost=   0.000090585;
  costs->key_lookup_cost=      0.000550142;
  costs->key_copy_cost=        0.000015685;
}


static int myisam_init(void *p)
{
  handlerton *hton;

  init_myisam_psi_keys();

  /* Set global variables based on startup options */
  if (myisam_recover_options && myisam_recover_options != HA_RECOVER_OFF)
    ha_open_options|=HA_OPEN_ABORT_IF_CRASHED;
  else
    myisam_recover_options= HA_RECOVER_OFF;

  myisam_block_size=(uint) 1 << my_bit_log2_uint64(opt_myisam_block_size);

  hton= (handlerton *)p;
  hton->db_type= DB_TYPE_MYISAM;
  hton->create= myisam_create_handler;
  hton->drop_table= myisam_drop_table;
  hton->panic= myisam_panic;
  hton->update_optimizer_costs= myisam_update_optimizer_costs;
  hton->flags= HTON_CAN_RECREATE | HTON_SUPPORT_LOG_TABLES;
  hton->tablefile_extensions= ha_myisam_exts;
  mi_killed= mi_killed_in_mariadb;

  return 0;
}

static struct st_mysql_sys_var* myisam_sysvars[]= {
  MYSQL_SYSVAR(block_size),
  MYSQL_SYSVAR(data_pointer_size),
  MYSQL_SYSVAR(max_sort_file_size),
  MYSQL_SYSVAR(recover_options),
  MYSQL_SYSVAR(repair_threads),
  MYSQL_SYSVAR(sort_buffer_size),
  MYSQL_SYSVAR(use_mmap),
  MYSQL_SYSVAR(mmap_size),
  MYSQL_SYSVAR(stats_method),
  0
};

/****************************************************************************
 * MyISAM MRR implementation: use DS-MRR
 ***************************************************************************/

int ha_myisam::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                     uint n_ranges, uint mode, 
                                     HANDLER_BUFFER *buf)
{
  return ds_mrr.dsmrr_init(this, seq, seq_init_param, n_ranges, mode, buf);
}

int ha_myisam::multi_range_read_next(range_id_t *range_info)
{
  return ds_mrr.dsmrr_next(range_info);
}

ha_rows ha_myisam::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                               void *seq_init_param, 
                                               uint n_ranges, uint *bufsz,
                                               uint *flags, ha_rows limit,
                                               Cost_estimate *cost)
{
  /*
    This call is here because there is no location where this->table would
    already be known.
    TODO: consider moving it into some per-query initialization call.
  */
  ds_mrr.init(this, table);
  return ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges, bufsz,
                                 flags, limit, cost);
}

ha_rows ha_myisam::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                         uint key_parts, uint *bufsz, 
                                         uint *flags, Cost_estimate *cost)
{
  ds_mrr.init(this, table);
  return ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz, flags, cost);
}


int ha_myisam::multi_range_read_explain_info(uint mrr_mode, char *str, 
                                             size_t size)
{
  return ds_mrr.dsmrr_explain_info(mrr_mode, str, size);
}

/* MyISAM MRR implementation ends */


/* Index condition pushdown implementation*/


Item *ha_myisam::idx_cond_push(uint keyno_arg, Item* idx_cond_arg)
{
  /*
    Check if the key contains a blob field. If it does then MyISAM
    should not accept the pushed index condition since MyISAM will not
    read the blob field from the index entry during evaluation of the
    pushed index condition and the BLOB field might be part of the
    range evaluation done by the ICP code.
  */
  const KEY *key= &table_share->key_info[keyno_arg];

  for (uint k= 0; k < key->user_defined_key_parts; ++k)
  {
    const KEY_PART_INFO *key_part= &key->key_part[k];
    if (key_part->key_part_flag & HA_BLOB_PART)
    {
      /* Let the server handle the index condition */
      return idx_cond_arg;
    }
  }

  pushed_idx_cond_keyno= keyno_arg;
  pushed_idx_cond= idx_cond_arg;
  in_range_check_pushed_down= TRUE;
  if (active_index == pushed_idx_cond_keyno)
    mi_set_index_cond_func(file, handler_index_cond_check, this);
  return NULL;
}

bool ha_myisam::rowid_filter_push(Rowid_filter* rowid_filter)
{
  /* This will be used in index_init() */
  pushed_rowid_filter= rowid_filter;
  return false;
}


/* Enable / disable rowid filter depending if it's active or not */

void ha_myisam::rowid_filter_changed()
{
  if (pushed_rowid_filter && handler_rowid_filter_is_active(this))
    mi_set_rowid_filter_func(file, handler_rowid_filter_check, this);
  else
    mi_set_rowid_filter_func(file, NULL, this);
}


struct st_mysql_storage_engine myisam_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(myisam)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &myisam_storage_engine,
  "MyISAM",
  "MySQL AB",
  "Non-transactional engine with good performance and small data footprint",
  PLUGIN_LICENSE_GPL,
  myisam_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  0x0100, /* 1.0 */
  NULL,                       /* status variables                */
  myisam_sysvars,             /* system variables                */
  "1.0",                      /* string version */
  MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
}
maria_declare_plugin_end;


/**
  @brief Register a named table with a call back function to the query cache.

  @param thd The thread handle
  @param table_key A pointer to the table name in the table cache
  @param key_length The length of the table name
  @param[out] engine_callback The pointer to the storage engine call back
    function, currently 0
  @param[out] engine_data Engine data will be set to 0.

  @note Despite the name of this function, it is used to check each statement
    before it is cached and not to register a table or callback function.

  @see handler::register_query_cache_table

  @return The error code. The engine_data and engine_callback will be set to 0.
    @retval TRUE Success
    @retval FALSE An error occurred
*/

my_bool ha_myisam::register_query_cache_table(THD *thd, const char *table_name,
                                              uint table_name_len,
                                              qc_engine_callback
                                              *engine_callback,
                                              ulonglong *engine_data)
{
  DBUG_ENTER("ha_myisam::register_query_cache_table");
  /*
    No call back function is needed to determine if a cached statement
    is valid or not.
  */
  *engine_callback= 0;

  /*
    No engine data is needed.
  */
  *engine_data= 0;

  if (file->s->concurrent_insert)
  {
    /*
      If a concurrent INSERT has happened just before the currently
      processed SELECT statement, the total size of the table is
      unknown.

      To determine if the table size is known, the current thread's snap
      shot of the table size with the actual table size are compared.

      If the table size is unknown the SELECT statement can't be cached.

      When concurrent inserts are disabled at table open, mi_ondopen()
      does not assign a get_status() function. In this case the local
      ("current") status is never updated. We would wrongly think that
      we cannot cache the statement.
    */
    ulonglong actual_data_file_length;
    ulonglong current_data_file_length;

    /*
      POSIX visibility rules specify that "2. Whatever memory values a
      thread can see when it unlocks a mutex <...> can also be seen by any
      thread that later locks the same mutex". In this particular case,
      concurrent insert thread had modified the data_file_length in
      MYISAM_SHARE before it has unlocked (or even locked)
      structure_guard_mutex. So, here we're guaranteed to see at least that
      value after we've locked the same mutex. We can see a later value
      (modified by some other thread) though, but it's ok, as we only want
      to know if the variable was changed, the actual new value doesn't matter
    */
    actual_data_file_length= file->s->state.state.data_file_length;
    current_data_file_length= file->save_state.data_file_length;

    if (current_data_file_length != actual_data_file_length)
    {
      /* Don't cache current statement. */
      DBUG_RETURN(FALSE);
    }
  }

  /*
    This query execution might have started after the query cache was flushed
    by a concurrent INSERT. In this case, don't cache this statement as the
    data file length difference might not be visible yet if the tables haven't
    been unlocked by the concurrent insert thread.
  */
  if (file->state->uncacheable)
    DBUG_RETURN(FALSE);

  /* It is ok to try to cache current statement. */
  DBUG_RETURN(TRUE);
}
