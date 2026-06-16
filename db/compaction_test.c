/* Copyright (c) 2011 The LevelDB Authors. All rights reserved.
   Use of this source code is governed by a BSD-style license that can be
   found in the LICENSE file. See the AUTHORS file for names of contributors. */
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

#include "rocksdb/c.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifndef OS_WIN
#include <unistd.h>
#endif
#include <inttypes.h>
#include <time.h>

// Can not use port/port.h macros as this is a c file
#ifdef OS_WIN
#include <windows.h>

// Ok for uniqueness
int geteuid() {
  int result = 0;

  result = ((int)GetCurrentProcessId() << 16);
  result |= (int)GetCurrentThreadId();

  return result;
}

#endif

const char* phase = "";
static char dbname[200];

static void StartPhase(const char* name) {
  fprintf(stderr, "=== Test %s\n", name);
  phase = name;
}
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv security warning
#endif
static const char* GetTempDir(void) {
  const char* ret = "/mnt/nvme/";
  return ret;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define CheckNoError(err)                                                 \
  if ((err) != NULL) {                                                    \
    fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, phase, (err)); \
    abort();                                                              \
  }

#define CheckCondition(cond)                                              \
  if (!(cond)) {                                                          \
    fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, phase, #cond); \
    abort();                                                              \
  }

static void CheckEqual(const char* expected, const char* v, size_t n) {
  if (expected == NULL && v == NULL) {
    // ok
  } else if (expected != NULL && v != NULL && n == strlen(expected) &&
             memcmp(expected, v, n) == 0) {
    // ok
    return;
  } else {
    fprintf(stderr, "%s: expected '%s', got '%s'\n", phase,
            (expected ? expected : "(null)"), (v ? v : "(null)"));
    abort();
  }
}

static void Free(char** ptr) {
  if (*ptr) {
    free(*ptr);
    *ptr = NULL;
  }
}

static void CheckValue(char* err, const char* expected, char** actual,
                       size_t actual_length) {
  CheckNoError(err);
  CheckEqual(expected, *actual, actual_length);
  Free(actual);
}

static void CheckGet(rocksdb_t* db, const rocksdb_readoptions_t* options,
                     const char* key, const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_get(db, options, key, strlen(key), &val_len, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckGetCF(rocksdb_t* db, const rocksdb_readoptions_t* options,
                       rocksdb_column_family_handle_t* handle, const char* key,
                       const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_get_cf(db, options, handle, key, strlen(key), &val_len, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckPinGet(rocksdb_t* db, const rocksdb_readoptions_t* options,
                        const char* key, const char* expected) {
  char* err = NULL;
  size_t val_len;
  const char* val;
  rocksdb_pinnableslice_t* p;
  p = rocksdb_get_pinned(db, options, key, strlen(key), &err);
  CheckNoError(err);
  val = rocksdb_pinnableslice_value(p, &val_len);
  CheckEqual(expected, val, val_len);
  rocksdb_pinnableslice_destroy(p);
}

static void CheckPinGetCF(rocksdb_t* db, const rocksdb_readoptions_t* options,
                          rocksdb_column_family_handle_t* handle,
                          const char* key, const char* expected) {
  char* err = NULL;
  size_t val_len;
  const char* val;
  rocksdb_pinnableslice_t* p;
  p = rocksdb_get_pinned_cf(db, options, handle, key, strlen(key), &err);
  CheckNoError(err);
  val = rocksdb_pinnableslice_value(p, &val_len);
  CheckEqual(expected, val, val_len);
  rocksdb_pinnableslice_destroy(p);
}

static void CheckMultiGetValues(size_t num_keys, char** values,
                                size_t* values_sizes, char** errs,
                                const char** expected) {
  for (size_t i = 0; i < num_keys; i++) {
    CheckNoError(errs[i]);
    CheckEqual(expected[i], values[i], values_sizes[i]);
    Free(&values[i]);
  }
}

static void CheckIter(rocksdb_iterator_t* iter, const char* key,
                      const char* val) {
  size_t len;
  const char* str;
  str = rocksdb_iter_key(iter, &len);
  CheckEqual(key, str, len);
  str = rocksdb_iter_value(iter, &len);
  CheckEqual(val, str, len);
}

// Callback from rocksdb_writebatch_iterate()
static void CheckPut(void* ptr, const char* k, size_t klen, const char* v,
                     size_t vlen) {
  int* state = (int*)ptr;
  CheckCondition(*state < 2);
  switch (*state) {
    case 0:
      CheckEqual("bar", k, klen);
      CheckEqual("b", v, vlen);
      break;
    case 1:
      CheckEqual("box", k, klen);
      CheckEqual("c", v, vlen);
      break;
  }
  (*state)++;
}

// Callback from rocksdb_writebatch_iterate()
static void CheckDel(void* ptr, const char* k, size_t klen) {
  int* state = (int*)ptr;
  CheckCondition(*state == 2);
  CheckEqual("bar", k, klen);
  (*state)++;
}

static void CmpDestroy(void* arg) { (void)arg; }

static int CmpCompare(void* arg, const char* a, size_t alen, const char* b,
                      size_t blen) {
  (void)arg;
  size_t n = (alen < blen) ? alen : blen;
  int r = memcmp(a, b, n);
  if (r == 0) {
    if (alen < blen) {
      r = -1;
    } else if (alen > blen) {
      r = +1;
    }
  }
  return r;
}

static const char* CmpName(void* arg) {
  (void)arg;
  return "foo";
}

// Custom compaction filter
static void CFilterDestroy(void* arg) { (void)arg; }
static const char* CFilterName(void* arg) {
  (void)arg;
  return "foo";
}
static unsigned char CFilterFilter(void* arg, int level, const char* key,
                                   size_t key_length,
                                   const char* existing_value,
                                   size_t value_length, char** new_value,
                                   size_t* new_value_length,
                                   unsigned char* value_changed) {
  (void)arg;
  (void)level;
  (void)existing_value;
  (void)value_length;
  if (key_length == 3) {
    if (memcmp(key, "bar", key_length) == 0) {
      return 1;
    } else if (memcmp(key, "baz", key_length) == 0) {
      *value_changed = 1;
      *new_value = "newbazvalue";
      *new_value_length = 11;
      return 0;
    }
  }
  return 0;
}

static void CFilterFactoryDestroy(void* arg) { (void)arg; }
static const char* CFilterFactoryName(void* arg) {
  (void)arg;
  return "foo";
}
static rocksdb_compactionfilter_t* CFilterCreate(
    void* arg, rocksdb_compactionfiltercontext_t* context) {
  (void)arg;
  (void)context;
  return rocksdb_compactionfilter_create(NULL, CFilterDestroy, CFilterFilter,
                                         CFilterName);
}

void CheckMetaData(rocksdb_column_family_metadata_t* cf_meta,
                   const char* expected_cf_name) {
  char* cf_name = rocksdb_column_family_metadata_get_name(cf_meta);
  assert(strcmp(cf_name, expected_cf_name) == 0);
  rocksdb_free(cf_name);

  size_t cf_size = rocksdb_column_family_metadata_get_size(cf_meta);
  assert(cf_size > 0);
  size_t cf_file_count = rocksdb_column_family_metadata_get_size(cf_meta);
  assert(cf_file_count > 0);

  uint64_t total_level_size = 0;
  size_t total_file_count = 0;
  size_t level_count = rocksdb_column_family_metadata_get_level_count(cf_meta);
  assert(level_count > 0);
  for (size_t l = 0; l < level_count; ++l) {
    rocksdb_level_metadata_t* level_meta =
        rocksdb_column_family_metadata_get_level_metadata(cf_meta, l);
    assert(level_meta);
    assert(rocksdb_level_metadata_get_level(level_meta) >= (int)l);
    uint64_t level_size = rocksdb_level_metadata_get_size(level_meta);
    uint64_t file_size_in_level = 0;

    size_t file_count = rocksdb_level_metadata_get_file_count(level_meta);
    total_file_count += file_count;
    for (size_t f = 0; f < file_count; ++f) {
      rocksdb_sst_file_metadata_t* file_meta =
          rocksdb_level_metadata_get_sst_file_metadata(level_meta, f);
      assert(file_meta);

      uint64_t file_size = rocksdb_sst_file_metadata_get_size(file_meta);
      assert(file_size > 0);
      file_size_in_level += file_size;

      char* file_name =
          rocksdb_sst_file_metadata_get_relative_filename(file_meta);
      assert(file_name);
      assert(strlen(file_name) > 0);
      rocksdb_free(file_name);

      size_t smallest_key_len;
      char* smallest_key = rocksdb_sst_file_metadata_get_smallestkey(
          file_meta, &smallest_key_len);
      assert(smallest_key);
      assert(smallest_key_len > 0);
      size_t largest_key_len;
      char* largest_key =
          rocksdb_sst_file_metadata_get_largestkey(file_meta, &largest_key_len);
      assert(largest_key);
      assert(largest_key_len > 0);
      rocksdb_free(smallest_key);
      rocksdb_free(largest_key);

      rocksdb_sst_file_metadata_destroy(file_meta);
    }
    assert(level_size == file_size_in_level);
    total_level_size += level_size;
    rocksdb_level_metadata_destroy(level_meta);
  }
  assert(total_file_count > 0);
  assert(cf_size == total_level_size);
}

void GetAndCheckMetaData(rocksdb_t* db) {
  rocksdb_column_family_metadata_t* cf_meta =
      rocksdb_get_column_family_metadata(db);

  CheckMetaData(cf_meta, "default");

  rocksdb_column_family_metadata_destroy(cf_meta);
}

void GetAndCheckMetaDataCf(rocksdb_t* db,
                           rocksdb_column_family_handle_t* handle,
                           const char* cf_name) {
  // Compact to make sure we have at least one sst file to obtain datadata.
  rocksdb_compact_range_cf(db, handle, NULL, 0, NULL, 0);

  rocksdb_column_family_metadata_t* cf_meta =
      rocksdb_get_column_family_metadata_cf(db, handle);

  CheckMetaData(cf_meta, cf_name);

  rocksdb_column_family_metadata_destroy(cf_meta);
}

static rocksdb_t* CheckCompaction(rocksdb_t* db, rocksdb_options_t* options,
                                  rocksdb_readoptions_t* roptions,
                                  rocksdb_writeoptions_t* woptions) {
  char* err = NULL;
  db = rocksdb_open(options, dbname, &err);
  CheckNoError(err);
  rocksdb_put(db, woptions, "foo", 3, "foovalue", 8, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "foo", "foovalue");
  rocksdb_put(db, woptions, "bar", 3, "barvalue", 8, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "bar", "barvalue");
  rocksdb_put(db, woptions, "baz", 3, "bazvalue", 8, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "baz", "bazvalue");

  // Disable compaction
  rocksdb_disable_manual_compaction(db);
  rocksdb_compact_range(db, NULL, 0, NULL, 0);
  // should not filter anything when disabled
  CheckGet(db, roptions, "foo", "foovalue");
  CheckGet(db, roptions, "bar", "barvalue");
  CheckGet(db, roptions, "baz", "bazvalue");
  // Reenable compaction
  rocksdb_enable_manual_compaction(db);

  // Force compaction
  rocksdb_compact_range(db, NULL, 0, NULL, 0);
  rocksdb_wait_for_compact_options_t* wco;
  wco = rocksdb_wait_for_compact_options_create();
  rocksdb_wait_for_compact(db, wco, &err);
  CheckNoError(err);
  rocksdb_wait_for_compact_options_destroy(wco);
  // should have filtered bar, but not foo
  CheckGet(db, roptions, "foo", "foovalue");
  CheckGet(db, roptions, "bar", NULL);
  CheckGet(db, roptions, "baz", "newbazvalue");

  rocksdb_suggest_compact_range(db, "bar", 3, "foo", 3, &err);
  GetAndCheckMetaData(db);
  CheckNoError(err);

  return db;
}

// Custom merge operator
static void MergeOperatorDestroy(void* arg) { (void)arg; }
static const char* MergeOperatorName(void* arg) {
  (void)arg;
  return "TestMergeOperator";
}
static char* MergeOperatorFullMerge(
    void* arg, const char* key, size_t key_length, const char* existing_value,
    size_t existing_value_length, const char* const* operands_list,
    const size_t* operands_list_length, int num_operands,
    unsigned char* success, size_t* new_value_length) {
  (void)arg;
  (void)key;
  (void)key_length;
  (void)existing_value;
  (void)existing_value_length;
  (void)operands_list;
  (void)operands_list_length;
  (void)num_operands;
  *new_value_length = 4;
  *success = 1;
  char* result = malloc(4);
  memcpy(result, "fake", 4);
  return result;
}
static char* MergeOperatorPartialMerge(void* arg, const char* key,
                                       size_t key_length,
                                       const char* const* operands_list,
                                       const size_t* operands_list_length,
                                       int num_operands, unsigned char* success,
                                       size_t* new_value_length) {
  (void)arg;
  (void)key;
  (void)key_length;
  (void)operands_list;
  (void)operands_list_length;
  (void)num_operands;
  *new_value_length = 4;
  *success = 1;
  char* result = malloc(4);
  memcpy(result, "fake", 4);
  return result;
}

static void CheckTxnGet(rocksdb_transaction_t* txn,
                        const rocksdb_readoptions_t* options, const char* key,
                        const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_transaction_get(txn, options, key, strlen(key), &val_len, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckTxnGetCF(rocksdb_transaction_t* txn,
                          const rocksdb_readoptions_t* options,
                          rocksdb_column_family_handle_t* column_family,
                          const char* key, const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_transaction_get_cf(txn, options, column_family, key,
                                   strlen(key), &val_len, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckTxnPinGet(rocksdb_transaction_t* txn,
                           const rocksdb_readoptions_t* options,
                           const char* key, const char* expected) {
  rocksdb_pinnableslice_t* p = NULL;
  const char* val = NULL;
  char* err = NULL;
  size_t val_len;
  p = rocksdb_transaction_get_pinned(txn, options, key, strlen(key), &err);
  CheckNoError(err);
  val = rocksdb_pinnableslice_value(p, &val_len);
  CheckEqual(expected, val, val_len);
  rocksdb_pinnableslice_destroy(p);
}

static void CheckTxnPinGetCF(rocksdb_transaction_t* txn,
                             const rocksdb_readoptions_t* options,
                             rocksdb_column_family_handle_t* column_family,
                             const char* key, const char* expected) {
  rocksdb_pinnableslice_t* p = NULL;
  const char* val = NULL;
  char* err = NULL;
  size_t val_len;
  p = rocksdb_transaction_get_pinned_cf(txn, options, column_family, key,
                                        strlen(key), &err);
  CheckNoError(err);
  val = rocksdb_pinnableslice_value(p, &val_len);
  CheckEqual(expected, val, val_len);
  rocksdb_pinnableslice_destroy(p);
}

static void CheckTxnGetForUpdate(rocksdb_transaction_t* txn,
                                 const rocksdb_readoptions_t* options,
                                 const char* key, const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_transaction_get_for_update(txn, options, key, strlen(key),
                                           &val_len, true, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckTxnDBGet(rocksdb_transactiondb_t* txn_db,
                          const rocksdb_readoptions_t* options, const char* key,
                          const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_transactiondb_get(txn_db, options, key, strlen(key), &val_len,
                                  &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckTxnDBGetCF(rocksdb_transactiondb_t* txn_db,
                            const rocksdb_readoptions_t* options,
                            rocksdb_column_family_handle_t* column_family,
                            const char* key, const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_transactiondb_get_cf(txn_db, options, column_family, key,
                                     strlen(key), &val_len, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckTxnGetForUpdateCF(
    rocksdb_transaction_t* txn, const rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t* column_family, const char* key,
    const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_transaction_get_for_update_cf(
      txn, options, column_family, key, strlen(key), &val_len, true, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckTxnDBPinGet(rocksdb_transactiondb_t* txn_db,
                             const rocksdb_readoptions_t* options,
                             const char* key, const char* expected) {
  rocksdb_pinnableslice_t* p = NULL;
  const char* val = NULL;
  char* err = NULL;
  size_t val_len;
  p = rocksdb_transactiondb_get_pinned(txn_db, options, key, strlen(key), &err);
  CheckNoError(err);
  val = rocksdb_pinnableslice_value(p, &val_len);
  CheckEqual(expected, val, val_len);
  rocksdb_pinnableslice_destroy(p);
}

static void CheckTxnDBPinGetCF(rocksdb_transactiondb_t* txn_db,
                               const rocksdb_readoptions_t* options,
                               rocksdb_column_family_handle_t* column_family,
                               const char* key, const char* expected) {
  rocksdb_pinnableslice_t* p = NULL;
  const char* val = NULL;
  char* err = NULL;
  size_t val_len;
  p = rocksdb_transactiondb_get_pinned_cf(txn_db, options, column_family, key,
                                          strlen(key), &err);
  CheckNoError(err);
  val = rocksdb_pinnableslice_value(p, &val_len);
  CheckEqual(expected, val, val_len);
  rocksdb_pinnableslice_destroy(p);
}

static void LoadAndCheckLatestOptions(const char* db_name, rocksdb_env_t* env,
                                      bool ignore_unknown_options,
                                      rocksdb_cache_t* cache,
                                      rocksdb_comparator_t* cmp,
                                      const size_t expected_num_column_families,
                                      const char** expected_cf_names,
                                      const char* expected_open_err) {
  rocksdb_options_t* db_options;
  size_t num_column_families;
  char** list_column_family_names;
  rocksdb_options_t** list_column_family_options;
  char* err = 0;

  // load the latest rocksdb option
  rocksdb_load_latest_options(db_name, env, ignore_unknown_options, cache,
                              &db_options, &num_column_families,
                              &list_column_family_names,
                              &list_column_family_options, &err);
  assert(num_column_families == expected_num_column_families);
  CheckNoError(err);

  // verify the loaded options by opening the db.
  rocksdb_options_set_error_if_exists(db_options, 0);

  char** list_const_cf_names =
      (char**)malloc(num_column_families * sizeof(char*));
  rocksdb_options_t** list_const_cf_options = (rocksdb_options_t**)malloc(
      num_column_families * sizeof(rocksdb_options_t*));
  for (size_t i = 0; i < num_column_families; ++i) {
    assert(strcmp(list_column_family_names[i], expected_cf_names[i]) == 0);
    list_const_cf_names[i] = list_column_family_names[i];
    if (cmp) {
      rocksdb_options_set_comparator(list_column_family_options[i], cmp);
    }
    list_const_cf_options[i] = list_column_family_options[i];
  }
  rocksdb_column_family_handle_t** handles =
      (rocksdb_column_family_handle_t**)malloc(
          num_column_families * sizeof(rocksdb_column_family_handle_t*));

  rocksdb_t* db = rocksdb_open_column_families(
      db_options, db_name, (int)num_column_families,
      (const char* const*)list_const_cf_names,
      (const rocksdb_options_t* const*)list_const_cf_options, handles, &err);
  if (expected_open_err == NULL) {
    CheckNoError(err);
    for (size_t i = 0; i < num_column_families; ++i) {
      rocksdb_column_family_handle_destroy(handles[i]);
    }
    free(handles);
    rocksdb_close(db);
  } else {
    assert(err != NULL);
    assert(strcmp(err, expected_open_err) == 0);
    free(handles);
    free(err);
  }

  free(list_const_cf_names);
  free(list_const_cf_options);
  rocksdb_load_latest_options_destroy(db_options, list_column_family_names,
                                      list_column_family_options,
                                      num_column_families);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  rocksdb_t* db;
  rocksdb_comparator_t* cmp;
  rocksdb_cache_t* cache;
  rocksdb_dbpath_t* dbpath;
  rocksdb_env_t* env;
  rocksdb_options_t* options;
  rocksdb_compactoptions_t* coptions;
  rocksdb_block_based_table_options_t* table_options;
  rocksdb_readoptions_t* roptions;
  rocksdb_writeoptions_t* woptions;
  char* err = NULL;
  bool load_flag = true;

  struct timespec prog_start, prog_end;
  unsigned long long start, end;

  snprintf(dbname, sizeof(dbname), "%s/rocks", GetTempDir());

  StartPhase("create_objects");
  env = rocksdb_create_default_env();

  rocksdb_create_dir_if_missing(env, GetTempDir(), &err);
  CheckNoError(err);

  cache = rocksdb_cache_create_lru(100000);

  options = rocksdb_options_create();
  rocksdb_options_set_error_if_exists(options, 1);
  rocksdb_options_set_env(options, env);
  rocksdb_options_set_info_log(options, NULL);
  rocksdb_options_set_csd_offload(options, 0);

//  rocksdb_options_set_write_buffer_size(options, 100000);
//  rocksdb_options_set_max_open_files(options, 10);
  rocksdb_options_set_max_bytes_for_level_base(options, 10*1024*1024); // for fast compaction trigger
//   rocksdb_options_enable_statistics(options);

  table_options = rocksdb_block_based_options_create();

  rocksdb_options_set_compression(options, rocksdb_no_compression);

  roptions = rocksdb_readoptions_create();
  rocksdb_readoptions_set_verify_checksums(roptions, 1);
  rocksdb_readoptions_set_fill_cache(roptions, 1);
  // rocksdb_readoptions_set_read_offload(roptions, 1);

  woptions = rocksdb_writeoptions_create();
  rocksdb_writeoptions_set_sync(woptions, 1);

  coptions = rocksdb_compactoptions_create();

  if (load_flag) {
    StartPhase("destroy");
    rocksdb_destroy_db(options, dbname, &err);
    Free(&err);
  }

  StartPhase("open_error");
  rocksdb_open(options, dbname, &err);
  CheckCondition(err != NULL);
  Free(&err);

  StartPhase("open");
  rocksdb_options_set_create_if_missing(options, 1);
  if (!load_flag) {
    rocksdb_options_set_error_if_exists(options, 0);
  }
  db = rocksdb_open(options, dbname, &err);
  CheckNoError(err);
//  CheckGet(db, roptions, "foo", NULL);

  char key[17];
  char value[4097];
  int load_num = 1000000;

  memset(value, 'x', 4096);
  value[4096] = '\0';

  // Seed random number generator for reproducibility
  srand(42);

  // Generate array of random keys
  int* random_keys = (int*)malloc(load_num * sizeof(int));
  for (int i = 0; i < load_num; i++) {
    random_keys[i] = i;
  }

  // Fisher-Yates shuffle for uniform randomness
  for (int i = load_num - 1; i > 0; i--) {
    int j = rand() % (i + 1);
    int temp = random_keys[i];
    random_keys[i] = random_keys[j];
    random_keys[j] = temp;
  }

  if (load_flag) {
    StartPhase("put");
    for (int i = 0; i < load_num; i++) {
      memset(key, 0, 17);
      sprintf(key, "key%03d", random_keys[i]);
      rocksdb_put(db, woptions, key, 16, value, 4096, &err);
      CheckNoError(err);
      if (i % 10000 == 0) {
        printf("Finished writing %d numbers\n", i);
      }
    }
  }

  /* Perform read to check if every entry is valid */
  char* val;
  size_t val_len;
  StartPhase("get");
  for (int i = 0; i < 1; i++) {
    memset(key, 0, 17);
    sprintf(key, "key%03d", random_keys[i]);
    printf("Get key: %s\n", key);
    val = rocksdb_get(db, roptions, key, 16, &val_len, &err);
    CheckNoError(err);
    CheckEqual(value, val, val_len);
    Free(&val);
    if (i % 1000 == 0) {
      printf("Finished reading %d numbers\n", i);
    }
  }

  free(random_keys);

  StartPhase("cancel_all_background_work");
  rocksdb_cancel_all_background_work(db, 1);

  StartPhase("cleanup");
  rocksdb_close(db);
  rocksdb_options_destroy(options);
  rocksdb_block_based_options_destroy(table_options);
  rocksdb_readoptions_destroy(roptions);
  rocksdb_writeoptions_destroy(woptions);
  rocksdb_compactoptions_destroy(coptions);
  rocksdb_cache_destroy(cache);
  rocksdb_env_destroy(env);

  fprintf(stderr, "PASS\n");
  return 0;
}
