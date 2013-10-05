/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tifflike.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <glib.h>

#include <tiff.h>

#ifndef TIFF_VERSION_BIG
// tiff.h is from libtiff < 4
#define TIFF_VERSION_CLASSIC TIFF_VERSION
#define TIFF_VERSION_BIG 43
#define TIFF_LONG8 16
#define TIFF_SLONG8 17
#define TIFF_IFD8 18
#endif


struct _openslide_tifflike {
  char *filename;
  bool big_endian;
  GPtrArray *directories;
};

struct tiff_item {
  uint16_t type;
  int64_t count;
  uint64_t offset;
  void *value;
};


static void fix_byte_order(void *data, int32_t size, int64_t count,
                           bool big_endian) {
  switch (size) {
  case 1: {
    break;
  }
  case 2: {
    uint16_t *arr = data;
    for (int64_t i = 0; i < count; i++) {
      arr[i] = big_endian ? GUINT16_FROM_BE(arr[i]) : GUINT16_FROM_LE(arr[i]);
    }
    break;
  }
  case 4: {
    uint32_t *arr = data;
    for (int64_t i = 0; i < count; i++) {
      arr[i] = big_endian ? GUINT32_FROM_BE(arr[i]) : GUINT32_FROM_LE(arr[i]);
    }
    break;
  }
  case 8: {
    uint64_t *arr = data;
    for (int64_t i = 0; i < count; i++) {
      arr[i] = big_endian ? GUINT64_FROM_BE(arr[i]) : GUINT64_FROM_LE(arr[i]);
    }
    break;
  }
  default:
    g_assert_not_reached();
    break;
  }
}

// only sets *ok on failure
static uint64_t read_uint(FILE *f, int32_t size, bool big_endian, bool *ok) {
  g_assert(ok != NULL);

  uint8_t buf[size];
  if (fread(buf, size, 1, f) != 1) {
    *ok = false;
    return 0;
  }
  fix_byte_order(buf, sizeof(buf), 1, big_endian);
  switch (size) {
  case 1: {
    uint8_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  case 2: {
    uint16_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  case 4: {
    uint32_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  case 8: {
    uint64_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  default:
    g_assert_not_reached();
  }
}

static uint32_t get_value_size(uint16_t type, uint64_t *count) {
  switch (type) {
  case TIFF_BYTE:
  case TIFF_ASCII:
  case TIFF_SBYTE:
  case TIFF_UNDEFINED:
    return 1;

  case TIFF_SHORT:
  case TIFF_SSHORT:
    return 2;

  case TIFF_LONG:
  case TIFF_SLONG:
  case TIFF_FLOAT:
  case TIFF_IFD:
    return 4;

  case TIFF_RATIONAL:
  case TIFF_SRATIONAL:
    *count *= 2;
    return 4;

  case TIFF_DOUBLE:
  case TIFF_LONG8:
  case TIFF_SLONG8:
  case TIFF_IFD8:
    return 8;

  default:
    return 0;
  }
}

static bool populate_item(struct _openslide_tifflike *tl,
                          struct tiff_item *item,
                          GError **err) {
  if (item->value) {
    return true;
  }

  FILE *f = _openslide_fopen(tl->filename, "rb", err);
  if (!f) {
    return false;
  }

  uint64_t count = item->count;
  int32_t value_size = get_value_size(item->type, &count);
  g_assert(value_size);
  ssize_t len = value_size * count;

  void *result = g_try_malloc(len);
  if (result == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot allocate TIFF value");
    goto FAIL;
  }

  //g_debug("reading tiff value: len: %"G_GINT64_FORMAT", offset %"G_GINT64_FORMAT, len, item->offset);
  if (fseeko(f, item->offset, SEEK_SET)) {
    _openslide_io_error(err, "Couldn't seek to read TIFF value");
    goto FAIL;
  }
  if (fread(result, len, 1, f) != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Couldn't read TIFF value");
    goto FAIL;
  }

  fix_byte_order(result, value_size, count, tl->big_endian);
  item->value = result;

  fclose(f);
  return true;

FAIL:
  g_free(result);
  fclose(f);
  return false;
}

static void tiff_item_destroy(gpointer data) {
  struct tiff_item *item = data;

  g_free(item->value);
  g_slice_free(struct tiff_item, item);
}

static GHashTable *read_directory(FILE *f, int64_t *diroff,
				  GHashTable *loop_detector,
				  bool bigtiff,
				  bool big_endian,
				  GError **err) {
  int64_t off = *diroff;
  *diroff = 0;
  GHashTable *result = NULL;
  bool ok = true;

  //  g_debug("diroff: %" PRId64, off);

  if (off <= 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Bad offset");
    goto FAIL;
  }

  // loop detection
  if (g_hash_table_lookup_extended(loop_detector, &off, NULL, NULL)) {
    // loop
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Loop detected");
    goto FAIL;
  }
  int64_t *key = g_slice_new(int64_t);
  *key = off;
  g_hash_table_insert(loop_detector, key, NULL);

  // no loop, let's seek
  if (fseeko(f, off, SEEK_SET) != 0) {
    _openslide_io_error(err, "Cannot seek to offset");
    goto FAIL;
  }

  // read directory count
  uint64_t dircount = read_uint(f, bigtiff ? 8 : 2, big_endian, &ok);
  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot read dircount");
    goto FAIL;
  }

  //  g_debug("dircount: %"G_GUINT64_FORMAT, dircount);


  // initial checks passed, initialize the hashtable
  result = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                 NULL, tiff_item_destroy);

  // read all directory entries
  for (uint64_t n = 0; n < dircount; n++) {
    uint16_t tag = read_uint(f, 2, big_endian, &ok);
    uint16_t type = read_uint(f, 2, big_endian, &ok);
    uint64_t count = read_uint(f, bigtiff ? 8 : 4, big_endian, &ok);

    if (!ok) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot read tag, type, and count");
      goto FAIL;
    }

    //    g_debug(" tag: %d, type: %d, count: %" PRId64, tag, type, count);

    // allocate the item
    struct tiff_item *item = g_slice_new0(struct tiff_item);
    item->type = type;
    item->count = count;
    g_hash_table_insert(result, GINT_TO_POINTER(tag), item);

    // compute value size
    uint32_t value_size = get_value_size(type, &count);
    if (!value_size) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unknown type encountered: %d", type);
      goto FAIL;
    }

    // check for overflow
    if (count > SSIZE_MAX / value_size) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Value count too large");
      goto FAIL;
    }

    // read in the value/offset
    uint8_t value[bigtiff ? 8 : 4];
    if (fread(value, sizeof(value), 1, f) != 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot read value/offset");
      goto FAIL;
    }

    // does value/offset contain the value?
    if (value_size * count <= sizeof(value)) {
      // yes
      item->value = g_memdup(value, value_size * count);
      fix_byte_order(item->value, value_size, count, big_endian);

    } else {
      // no; store offset
      if (bigtiff) {
        memcpy(&item->offset, value, 8);
        fix_byte_order(&item->offset, sizeof(item->offset), 1, big_endian);
      } else {
        uint32_t off32;
        memcpy(&off32, value, 4);
        fix_byte_order(&off32, sizeof(off32), 1, big_endian);
        item->offset = off32;
      }
    }
  }

  // read the next dir offset
  int64_t nextdiroff = read_uint(f, bigtiff ? 8 : 4, big_endian, &ok);
  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot read next directory offset");
    goto FAIL;
  }
  *diroff = nextdiroff;

  // success
  return result;


 FAIL:
  if (result != NULL) {
    g_hash_table_unref(result);
  }
  return NULL;
}

struct _openslide_tifflike *_openslide_tifflike_create(const char *filename,
                                                       GError **err) {
  struct _openslide_tifflike *tl = NULL;
  GHashTable *loop_detector = NULL;
  GError *tmp_err = NULL;

  // open file
  FILE *f = _openslide_fopen(filename, "rb", &tmp_err);
  if (!f) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s", tmp_err->message);
    g_clear_error(&tmp_err);
    goto FAIL;
  }

  // read and check magic
  uint16_t magic;
  fseeko(f, 0, SEEK_SET);
  if (fread(&magic, sizeof magic, 1, f) != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Can't read TIFF magic number");
    goto FAIL;
  }
  if (magic != TIFF_BIGENDIAN && magic != TIFF_LITTLEENDIAN) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unrecognized TIFF magic number");
    goto FAIL;
  }
  bool big_endian = (magic == TIFF_BIGENDIAN);

  //  g_debug("magic: %d", magic);

  // read rest of header
  bool ok = true;
  uint16_t version = read_uint(f, 2, big_endian, &ok);
  bool bigtiff = (version == TIFF_VERSION_BIG);
  uint16_t offset_size = 0;
  uint16_t pad = 0;
  if (bigtiff) {
    offset_size = read_uint(f, 2, big_endian, &ok);
    pad = read_uint(f, 2, big_endian, &ok);
  }
  int64_t diroff = read_uint(f, bigtiff ? 8 : 4, big_endian, &ok);

  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Can't read TIFF header");
    goto FAIL;
  }

  //  g_debug("version: %d", version);

  // validate
  if (version == TIFF_VERSION_BIG) {
    if (offset_size != 8 || pad != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                  "Unexpected value in BigTIFF header");
      goto FAIL;
    }
  } else if (version != TIFF_VERSION_CLASSIC) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unrecognized TIFF version");
    goto FAIL;
  }

  // allocate struct
  tl = g_slice_new0(struct _openslide_tifflike);
  tl->filename = g_strdup(filename);
  tl->big_endian = big_endian;
  tl->directories = g_ptr_array_new();

  // initialize loop detector
  loop_detector = g_hash_table_new_full(_openslide_int64_hash,
                                        _openslide_int64_equal,
                                        _openslide_int64_free,
                                        NULL);
  // read all the directories
  while (diroff != 0) {
    // read a directory
    GHashTable *ht = read_directory(f, &diroff, loop_detector, bigtiff,
                                    big_endian, err);

    // was the directory successfully read?
    if (ht == NULL) {
      goto FAIL;
    }

    // add result to array
    g_ptr_array_add(tl->directories, ht);
  }

  // ensure there are directories
  if (tl->directories->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "TIFF contains no directories");
    goto FAIL;
  }

  g_hash_table_unref(loop_detector);
  fclose(f);
  return tl;

FAIL:
  _openslide_tifflike_destroy(tl);
  if (loop_detector) {
    g_hash_table_unref(loop_detector);
  }
  if (f) {
    fclose(f);
  }
  return NULL;
}


void _openslide_tifflike_destroy(struct _openslide_tifflike *tl) {
  if (tl == NULL) {
    return;
  }
  for (uint32_t n = 0; n < tl->directories->len; n++) {
    g_hash_table_unref(tl->directories->pdata[n]);
  }
  g_ptr_array_free(tl->directories, true);
  g_free(tl->filename);
  g_slice_free(struct _openslide_tifflike, tl);
}

static struct tiff_item *get_item(struct _openslide_tifflike *tl,
                                  int64_t dir, int32_t tag) {
  if (dir < 0 || dir >= tl->directories->len) {
    return NULL;
  }
  return g_hash_table_lookup(tl->directories->pdata[dir],
                             GINT_TO_POINTER(tag));
}

static void print_tag(struct _openslide_tifflike *tl,
                      int64_t dir, int32_t tag) {
  struct tiff_item *item = get_item(tl, dir, tag);
  g_assert(item != NULL);

  printf(" %d: type: %d, count: %" G_GINT64_FORMAT "\n ", tag, item->type, item->count);

  if (item->type == TIFF_ASCII) {
    // will only print first string if there are multiple
    const char *str = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
    if (str[item->count - 1] != '\0') {
      str = "<not null-terminated>";
    }
    printf(" %s", str);
  } else if (item->type == TIFF_UNDEFINED) {
    const uint8_t *data = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
    for (int64_t i = 0; i < item->count; i++) {
      printf(" %u", data[i]);
    }
  } else {
    for (int64_t i = 0; i < item->count; i++) {
      switch (item->type) {
      case TIFF_BYTE:
      case TIFF_SHORT:
      case TIFF_LONG:
      case TIFF_LONG8:
	printf(" %" G_GUINT64_FORMAT,
	       _openslide_tifflike_get_uint(tl, dir, tag, i, NULL));
	break;

      case TIFF_IFD:
      case TIFF_IFD8:
	printf(" %.16" G_GINT64_MODIFIER "x",
	       _openslide_tifflike_get_uint(tl, dir, tag, i, NULL));
	break;

      case TIFF_SBYTE:
      case TIFF_SSHORT:
      case TIFF_SLONG:
      case TIFF_SLONG8:
	printf(" %" G_GINT64_FORMAT,
	       _openslide_tifflike_get_sint(tl, dir, tag, i, NULL));
	break;

      case TIFF_FLOAT:
      case TIFF_DOUBLE:
      case TIFF_RATIONAL:
      case TIFF_SRATIONAL:
	printf(" %g",
	       _openslide_tifflike_get_float(tl, dir, tag, i, NULL));
	break;

      default:
	g_return_if_reached();
      }
    }
  }
  printf("\n");
}

static int tag_compare(gconstpointer a, gconstpointer b) {
  int32_t aa = GPOINTER_TO_INT(a);
  int32_t bb = GPOINTER_TO_INT(b);

  if (aa < bb) {
    return -1;
  } else if (aa > bb) {
    return 1;
  } else {
    return 0;
  }
}

static void print_directory(struct _openslide_tifflike *tl,
                            int64_t dir) {
  GList *keys = g_hash_table_get_keys(tl->directories->pdata[dir]);
  keys = g_list_sort(keys, tag_compare);
  for (GList *el = keys; el; el = el->next) {
    print_tag(tl, dir, GPOINTER_TO_INT(el->data));
  }
  g_list_free(keys);

  printf("\n");
}

void _openslide_tifflike_print(struct _openslide_tifflike *tl) {
  for (uint32_t n = 0; n < tl->directories->len; n++) {
    printf("Directory %u\n", n);
    print_directory(tl, n);
  }
}

int64_t _openslide_tifflike_get_directory_count(struct _openslide_tifflike *tl) {
  return tl->directories->len;
}

int64_t _openslide_tifflike_get_value_count(struct _openslide_tifflike *tl,
                                            int64_t dir, int32_t tag) {
  struct tiff_item *item = get_item(tl, dir, tag);
  if (item == NULL) {
    return 0;
  }
  return item->count;
}

static struct tiff_item *get_and_check_item(struct _openslide_tifflike *tl,
                                            int64_t dir, int32_t tag,
                                            int64_t i, GError **err) {
  struct tiff_item *item = get_item(tl, dir, tag);
  if (item == NULL || i < 0 || i >= item->count) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE,
                "No such value: directory %"G_GINT64_FORMAT", tag %d, "
                "value %"G_GINT64_FORMAT, dir, tag, i);
    return NULL;
  }
  return item;
}

uint64_t _openslide_tifflike_get_uint(struct _openslide_tifflike *tl,
                                      int64_t dir, int32_t tag, int64_t i,
                                      GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, i, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return 0;
  }
  switch (item->type) {
  case TIFF_BYTE:
    return ((uint8_t *) item->value)[i];
  case TIFF_SHORT:
    return ((uint16_t *) item->value)[i];
  case TIFF_LONG:
  case TIFF_IFD:
    return ((uint32_t *) item->value)[i];
  case TIFF_LONG8:
  case TIFF_IFD8:
    return ((uint64_t *) item->value)[i];
  default:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unexpected value type: directory %"G_GINT64_FORMAT", "
                "tag %d, type %d", dir, tag, item->type);
    return 0;
  }
}

int64_t _openslide_tifflike_get_sint(struct _openslide_tifflike *tl,
                                     int64_t dir, int32_t tag, int64_t i,
                                     GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, i, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return 0;
  }
  switch (item->type) {
  case TIFF_SBYTE:
    return ((int8_t *) item->value)[i];
  case TIFF_SSHORT:
    return ((int16_t *) item->value)[i];
  case TIFF_SLONG:
    return ((int32_t *) item->value)[i];
  case TIFF_SLONG8:
    return ((int64_t *) item->value)[i];
  default:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unexpected value type: directory %"G_GINT64_FORMAT", "
                "tag %d, type %d", dir, tag, item->type);
    return 0;
  }
}

double _openslide_tifflike_get_float(struct _openslide_tifflike *tl,
                                     int64_t dir, int32_t tag, int64_t i,
                                     GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, i, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return NAN;
  }
  switch (item->type) {
  case TIFF_FLOAT: {
    float val;
    memcpy(&val, ((uint32_t *) item->value) + i, sizeof(val));
    return val;
  }
  case TIFF_DOUBLE: {
    double val;
    memcpy(&val, ((uint64_t *) item->value) + i, sizeof(val));
    return val;
  }
  case TIFF_RATIONAL: {
    // convert 2 longs into rational
    uint32_t *val = item->value;
    return (double) val[i * 2] / (double) val[i * 2 + 1];
  }
  case TIFF_SRATIONAL: {
    // convert 2 slongs into rational
    int32_t *val = item->value;
    return (double) val[i * 2] / (double) val[i * 2 + 1];
  }
  default:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unexpected value type: directory %"G_GINT64_FORMAT", "
                "tag %d, type %d", dir, tag, item->type);
    return NAN;
  }
}

const void *_openslide_tifflike_get_buffer(struct _openslide_tifflike *tl,
                                           int64_t dir, int32_t tag,
                                           GError **err) {
  struct tiff_item *item = get_item(tl, dir, tag);
  if (item == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE,
                "No such value: directory %"G_GINT64_FORMAT", tag %d",
                dir, tag);
    return NULL;
  }
  if (!populate_item(tl, item, err)) {
    return NULL;
  }
  switch (item->type) {
  case TIFF_ASCII:
  case TIFF_UNDEFINED:
    return item->value;
  default:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unexpected value type: directory %"G_GINT64_FORMAT", "
                "tag %d, type %d", dir, tag, item->type);
    return NULL;
  }
}

static const char *store_string_property(struct _openslide_tifflike *tl,
                                         int64_t dir,
                                         openslide_t *osr,
                                         const char *name,
                                         int32_t tag) {
  const char *buf = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
  if (!buf) {
    return NULL;
  }
  char *value = g_strdup(buf);
  g_hash_table_insert(osr->properties, g_strdup(name), value);
  return value;
}

static void store_and_hash_string_property(struct _openslide_tifflike *tl,
                                           int64_t dir,
                                           openslide_t *osr,
                                           struct _openslide_hash *quickhash1,
                                           const char *name,
                                           int32_t tag) {
  _openslide_hash_string(quickhash1, name);
  _openslide_hash_string(quickhash1,
                         store_string_property(tl, dir, osr, name, tag));
}

static void store_float_property(struct _openslide_tifflike *tl,
                                 int64_t dir,
                                 openslide_t *osr,
                                 const char *name,
                                 int32_t tag) {
  GError *tmp_err = NULL;
  double value = _openslide_tifflike_get_float(tl, dir, tag, 0, &tmp_err);
  if (!tmp_err) {
    g_hash_table_insert(osr->properties,
                        g_strdup(name),
                        _openslide_format_double(value));
  }
  g_clear_error(&tmp_err);
}

static void store_and_hash_properties(struct _openslide_tifflike *tl,
                                      int64_t dir,
                                      openslide_t *osr,
                                      struct _openslide_hash *quickhash1) {
  GError *tmp_err = NULL;

  // strings
  store_string_property(tl, dir, osr, OPENSLIDE_PROPERTY_NAME_COMMENT,
                        TIFFTAG_IMAGEDESCRIPTION);

  // strings to store and hash
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.ImageDescription",
                                 TIFFTAG_IMAGEDESCRIPTION);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Make", TIFFTAG_MAKE);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Model", TIFFTAG_MODEL);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Software", TIFFTAG_SOFTWARE);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.DateTime", TIFFTAG_DATETIME);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Artist", TIFFTAG_ARTIST);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.HostComputer", TIFFTAG_HOSTCOMPUTER);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Copyright", TIFFTAG_COPYRIGHT);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.DocumentName", TIFFTAG_DOCUMENTNAME);

  // don't hash floats, they might be unstable over time
  store_float_property(tl, dir, osr, "tiff.XResolution", TIFFTAG_XRESOLUTION);
  store_float_property(tl, dir, osr, "tiff.YResolution", TIFFTAG_YRESOLUTION);
  store_float_property(tl, dir, osr, "tiff.XPosition", TIFFTAG_XPOSITION);
  store_float_property(tl, dir, osr, "tiff.YPosition", TIFFTAG_YPOSITION);

  // special
  int64_t resolution_unit =
    _openslide_tifflike_get_uint(tl, dir, TIFFTAG_RESOLUTIONUNIT, 0, &tmp_err);
  if (tmp_err) {
    resolution_unit = RESUNIT_INCH;  // default
    g_clear_error(&tmp_err);
  }
  const char *result;
  switch(resolution_unit) {
  case RESUNIT_NONE:
    result = "none";
    break;
  case RESUNIT_INCH:
    result = "inch";
    break;
  case RESUNIT_CENTIMETER:
    result = "centimeter";
    break;
  default:
    result = "unknown";
  }
  g_hash_table_insert(osr->properties,
                      g_strdup("tiff.ResolutionUnit"),
                      g_strdup(result));
}

static bool hash_tiff_level(struct _openslide_hash *hash,
                            const char *filename,
                            struct _openslide_tifflike *tl,
                            int32_t dir,
                            GError **err) {
  GError *tmp_err = NULL;
  int32_t offset_tag;
  int32_t length_tag;

  // determine layout
  if (_openslide_tifflike_get_value_count(tl, dir, TIFFTAG_TILEOFFSETS)) {
    // tiled
    offset_tag = TIFFTAG_TILEOFFSETS;
    length_tag = TIFFTAG_TILEBYTECOUNTS;
  } else if (_openslide_tifflike_get_value_count(tl, dir,
                                                 TIFFTAG_STRIPOFFSETS)) {
    // stripped
    offset_tag = TIFFTAG_STRIPOFFSETS;
    length_tag = TIFFTAG_STRIPBYTECOUNTS;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Directory %d is neither tiled nor stripped", dir);
    return false;
  }

  // get tile/strip count
  int64_t count = _openslide_tifflike_get_value_count(tl, dir, offset_tag);
  if (!count ||
      count != _openslide_tifflike_get_value_count(tl, dir, length_tag)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid tile/strip counts for directory %d", dir);
    return false;
  }

  // check total size
  int64_t total = 0;
  for (int64_t i = 0; i < count; i++) {
    total += _openslide_tifflike_get_uint(tl, dir, length_tag, i, &tmp_err);
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
      return false;
    }
    if (total > (5 << 20)) {
      // This is a non-pyramidal image or one with a very large top level.
      // Refuse to calculate a quickhash for it to keep openslide_open()
      // from taking an arbitrary amount of time.  (#79)
      _openslide_hash_disable(hash);
      return true;
    }
  }

  // hash raw data of each tile/strip
  for (int64_t i = 0; i < count; i++) {
    int64_t offset = _openslide_tifflike_get_uint(tl, dir, offset_tag, i,
                                                  &tmp_err);
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
      return false;
    }
    int64_t length = _openslide_tifflike_get_uint(tl, dir, length_tag, i,
                                                  &tmp_err);
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
      return false;
    }
    if (!_openslide_hash_file_part(hash, filename, offset, length, err)) {
      return false;
    }
  }

  return true;
}

bool _openslide_tifflike_init_properties_and_hash(openslide_t *osr,
                                                  const char *filename,
                                                  struct _openslide_tifflike *tl,
                                                  struct _openslide_hash *quickhash1,
                                                  int32_t lowest_resolution_level,
                                                  int32_t property_dir,
                                                  GError **err) {
  // generate hash of the smallest level
  if (!hash_tiff_level(quickhash1, filename,
                       tl, lowest_resolution_level, err)) {
    g_prefix_error(err, "Cannot hash TIFF tiles: ");
    return false;
  }

  // load TIFF properties
  store_and_hash_properties(tl, property_dir, osr, quickhash1);

  return true;
}
