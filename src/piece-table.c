#include "piece-table.h"
#include "resource-check.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#ifdef HAVE_UCHARDET
#include <uchardet.h>
#endif

/* -- DiskBuffer: Zero-RAM disk-backed buffer using mmap -- */

typedef struct {
    int fd;
    char *path;
    char *mmap_base;
    size_t size;
} PieceTableSource;

#define DISK_BUFFER_INITIAL_SIZE 4096  /* 4KB initial, page-aligned */

#define UTF16_CONVERT_CHUNK_SIZE (512 * 1024)
#define UTF16_CONVERT_THROTTLE_WORK_US 20000
#define UTF16_CONVERT_THROTTLE_SLEEP_US 6000
#define UTF16_CONVERT_THROTTLE_MIN_BYTES (8 * 1024 * 1024)

static inline void
utf16_convert_throttle(size_t total_size, gint64 *work_budget_us)
{
    if (total_size < UTF16_CONVERT_THROTTLE_MIN_BYTES) return;
    if (*work_budget_us >= UTF16_CONVERT_THROTTLE_WORK_US) {
        g_usleep(UTF16_CONVERT_THROTTLE_SLEEP_US);
        *work_budget_us = 0;
    }
}

static DiskBuffer *
disk_buffer_new(void)
{
    DiskBuffer *buf = g_new0(DiskBuffer, 1);
    buf->fd = -1;
    buf->path = NULL;
    buf->mmap_base = NULL;
    buf->len = 0;
    buf->capacity = DISK_BUFFER_INITIAL_SIZE;
    
    const char *tmp_dir = g_get_tmp_dir();
    if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";

    /* Try O_TMPFILE first (anonymous temp file, no path needed) */
#ifdef O_TMPFILE
    buf->fd = open(tmp_dir, O_TMPFILE | O_RDWR | O_EXCL, 0600);
    if (buf->fd != -1) {
        buf->path = NULL;  /* No path needed for O_TMPFILE */
    } else
#endif
    {
        /* Fallback to mkstemp */
        buf->path = g_strdup_printf("%s/vite_add_buf_XXXXXX", tmp_dir);
        buf->fd = mkstemp(buf->path);
        if (buf->fd == -1) {
            g_warning("disk_buffer_new: Failed to create temp file: %s", strerror(errno));
            g_free(buf->path);
            g_free(buf);
            return NULL;
        }
        /* Unlink immediately so file is deleted when fd closes */
        unlink(buf->path);
    }
    
    /* Pre-allocate initial capacity */
    if (ftruncate(buf->fd, buf->capacity) == -1) {
        g_warning("disk_buffer_new: ftruncate failed: %s", strerror(errno));
        close(buf->fd);
        g_free(buf->path);
        g_free(buf);
        return NULL;
    }
    
    /* mmap the file for read/write access */
    buf->mmap_base = mmap(NULL, buf->capacity, PROT_READ | PROT_WRITE, MAP_SHARED, buf->fd, 0);
    if (buf->mmap_base == MAP_FAILED) {
        g_warning("disk_buffer_new: mmap failed: %s", strerror(errno));
        close(buf->fd);
        g_free(buf->path);
        g_free(buf);
        return NULL;
    }
    
    return buf;
}

static void insert_piece_at_offset(PieceTable *pt, size_t offset, Piece new_piece);

static void
disk_buffer_free(DiskBuffer *buf)
{
    if (!buf) return;
    
    if (buf->mmap_base && buf->mmap_base != MAP_FAILED && buf->capacity > 0) {
        munmap(buf->mmap_base, buf->capacity);
    }
    if (buf->fd >= 0) {
        close(buf->fd);
    }
    g_free(buf->path);
    g_free(buf);
}

static void
disk_buffer_append(DiskBuffer *buf, const char *data, size_t len)
{
    if (!buf || len == 0) return;
    
    /* Overflow check: ensure addition won't wrap around */
    if (len > SIZE_MAX - buf->len) {
        g_warning("disk_buffer_append: Size overflow detected (len=%zu, current=%zu)", len, buf->len);
        return;
    }
    
    /* Pre-check the size validity to catch sign extension */
    if (!resource_size_valid(len)) {
        g_warning("disk_buffer_append: Size invalid/overflow (len=%zu)", len);
        return;
    }
    
    size_t new_len = buf->len + len;
    const char *tmp_dir = g_get_tmp_dir();
    if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
    
    /* Sanity check for obviously corrupt/overflowed values */
    if (!resource_size_valid(new_len)) {
        g_warning("disk_buffer_append: Suspiciously large size %zu (likely overflow)", new_len);
        return;
    }
    
    /* Check disk space before growing */
    if (new_len > buf->capacity) {
        /* Calculate new capacity (double, but page-aligned) */
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->capacity) { /* Overflow in doubling */
            new_cap = new_len + DISK_BUFFER_INITIAL_SIZE;
        }
        if (new_cap < new_len) new_cap = new_len + DISK_BUFFER_INITIAL_SIZE;
        
        /* Page-align capacity */
        new_cap = (new_cap + 4095) & ~4095;
        
        /* Check disk space before allocation */
        if (!resource_can_write_disk(tmp_dir, new_cap - buf->capacity)) {
            g_warning("disk_buffer_append: Insufficient disk space for %zu bytes", new_cap);
            return;
        }
        
        /* Extend file */
        if (ftruncate(buf->fd, new_cap) == -1) {
            g_warning("disk_buffer_append: ftruncate failed: %s", strerror(errno));
            return;
        }
        
        /* Remap to new size using mremap (Linux-specific, very efficient) */
#ifdef MREMAP_MAYMOVE
        char *new_base = mremap(buf->mmap_base, buf->capacity, new_cap, MREMAP_MAYMOVE);
        if (new_base == MAP_FAILED) {
            g_warning("disk_buffer_append: mremap failed: %s", strerror(errno));
            return;
        }
        buf->mmap_base = new_base;
#else
        /* Fallback: munmap + mmap (portable but slightly slower) */
        munmap(buf->mmap_base, buf->capacity);
        buf->mmap_base = mmap(NULL, new_cap, PROT_READ | PROT_WRITE, MAP_SHARED, buf->fd, 0);
        if (buf->mmap_base == MAP_FAILED) {
            g_warning("disk_buffer_append: mmap failed after resize: %s", strerror(errno));
            return;
        }
#endif
        buf->capacity = new_cap;
    }
    
    /* Copy data to mmap'd region (kernel will flush to disk) */
    memcpy(buf->mmap_base + buf->len, data, len);
    buf->len = new_len;
}


/* -- Utils -- */

static const char *
get_piece_data(PieceTable *pt, const Piece *p)
{
    if (p->source == SOURCE_ORIGINAL) return pt->orig_data;
    if (p->source == SOURCE_ADD) return (const char *)pt->add_buffer->mmap_base;
    if (p->source >= SOURCE_EXTERNAL_START) {
        guint idx = p->source - SOURCE_EXTERNAL_START;
        if (pt->external_sources && idx < pt->external_sources->len) {
            PieceTableSource *src = g_ptr_array_index(pt->external_sources, idx);
            return src->mmap_base;
        }
    }
    return "";
}

typedef struct {
    FileEncoding enc;
    const char *id;
    const char *display;
    const char *charset;
    gboolean stream_safe;
} FileEncodingInfo;

static const FileEncodingInfo encoding_infos[] = {
    { ENCODING_UTF8,         "utf-8",         "UTF-8",         "UTF-8",         TRUE  },
    { ENCODING_UTF16LE,      "utf-16le",      "UTF-16 LE",     "UTF-16LE",      TRUE  },
    { ENCODING_UTF16BE,      "utf-16be",      "UTF-16 BE",     "UTF-16BE",      TRUE  },
    { ENCODING_UTF32LE,      "utf-32le",      "UTF-32 LE",     "UTF-32LE",      TRUE  },
    { ENCODING_UTF32BE,      "utf-32be",      "UTF-32 BE",     "UTF-32BE",      TRUE  },
    { ENCODING_ISO_8859_1,   "iso-8859-1",    "ISO-8859-1",    "ISO-8859-1",    TRUE  },
    { ENCODING_ISO_8859_2,   "iso-8859-2",    "ISO-8859-2",    "ISO-8859-2",    TRUE  },
    { ENCODING_ISO_8859_3,   "iso-8859-3",    "ISO-8859-3",    "ISO-8859-3",    TRUE  },
    { ENCODING_ISO_8859_4,   "iso-8859-4",    "ISO-8859-4",    "ISO-8859-4",    TRUE  },
    { ENCODING_ISO_8859_5,   "iso-8859-5",    "ISO-8859-5",    "ISO-8859-5",    TRUE  },
    { ENCODING_ISO_8859_6,   "iso-8859-6",    "ISO-8859-6",    "ISO-8859-6",    TRUE  },
    { ENCODING_ISO_8859_7,   "iso-8859-7",    "ISO-8859-7",    "ISO-8859-7",    TRUE  },
    { ENCODING_ISO_8859_8,   "iso-8859-8",    "ISO-8859-8",    "ISO-8859-8",    TRUE  },
    { ENCODING_ISO_8859_9,   "iso-8859-9",    "ISO-8859-9",    "ISO-8859-9",    TRUE  },
    { ENCODING_ISO_8859_10,  "iso-8859-10",   "ISO-8859-10",   "ISO-8859-10",   TRUE  },
    { ENCODING_ISO_8859_11,  "iso-8859-11",   "ISO-8859-11",   "ISO-8859-11",   TRUE  },
    { ENCODING_ISO_8859_13,  "iso-8859-13",   "ISO-8859-13",   "ISO-8859-13",   TRUE  },
    { ENCODING_ISO_8859_14,  "iso-8859-14",   "ISO-8859-14",   "ISO-8859-14",   TRUE  },
    { ENCODING_ISO_8859_15,  "iso-8859-15",   "ISO-8859-15",   "ISO-8859-15",   TRUE  },
    { ENCODING_ISO_8859_16,  "iso-8859-16",   "ISO-8859-16",   "ISO-8859-16",   TRUE  },
    { ENCODING_WINDOWS_1250, "windows-1250",  "Windows-1250",  "WINDOWS-1250",  TRUE  },
    { ENCODING_WINDOWS_1251, "windows-1251",  "Windows-1251",  "WINDOWS-1251",  TRUE  },
    { ENCODING_WINDOWS_1252, "windows-1252",  "Windows-1252",  "WINDOWS-1252",  TRUE  },
    { ENCODING_WINDOWS_1253, "windows-1253",  "Windows-1253",  "WINDOWS-1253",  TRUE  },
    { ENCODING_WINDOWS_1254, "windows-1254",  "Windows-1254",  "WINDOWS-1254",  TRUE  },
    { ENCODING_WINDOWS_1255, "windows-1255",  "Windows-1255",  "WINDOWS-1255",  TRUE  },
    { ENCODING_WINDOWS_1256, "windows-1256",  "Windows-1256",  "WINDOWS-1256",  TRUE  },
    { ENCODING_WINDOWS_1257, "windows-1257",  "Windows-1257",  "WINDOWS-1257",  TRUE  },
    { ENCODING_WINDOWS_1258, "windows-1258",  "Windows-1258",  "WINDOWS-1258",  TRUE  },
    { ENCODING_KOI8_R,       "koi8-r",        "KOI8-R",        "KOI8-R",        TRUE  },
    { ENCODING_KOI8_U,       "koi8-u",        "KOI8-U",        "KOI8-U",        TRUE  },
    { ENCODING_CP850,        "cp850",         "CP850",         "CP850",         TRUE  },
    { ENCODING_CP852,        "cp852",         "CP852",         "CP852",         TRUE  },
    { ENCODING_CP855,        "cp855",         "CP855",         "CP855",         TRUE  },
    { ENCODING_CP857,        "cp857",         "CP857",         "CP857",         TRUE  },
    { ENCODING_CP862,        "cp862",         "CP862",         "CP862",         TRUE  },
    { ENCODING_CP864,        "cp864",         "CP864",         "CP864",         TRUE  },
    { ENCODING_CP866,        "cp866",         "CP866",         "CP866",         TRUE  },
    { ENCODING_SHIFT_JIS,    "shift_jis",     "Shift_JIS",     "SHIFT_JIS",     FALSE },
    { ENCODING_EUC_JP,       "euc-jp",        "EUC-JP",        "EUC-JP",        FALSE },
    { ENCODING_ISO_2022_JP,  "iso-2022-jp",   "ISO-2022-JP",   "ISO-2022-JP",   FALSE },
    { ENCODING_GB18030,      "gb18030",       "GB18030",       "GB18030",       FALSE },
    { ENCODING_GBK,          "gbk",           "GBK",           "GBK",           FALSE },
    { ENCODING_BIG5,         "big5",          "Big5",          "BIG5",          FALSE },
    { ENCODING_BIG5_HKSCS,   "big5-hkscs",    "Big5-HKSCS",    "BIG5-HKSCS",    FALSE },
    { ENCODING_EUC_KR,       "euc-kr",        "EUC-KR",        "EUC-KR",        FALSE },
    { ENCODING_CP949,        "cp949",         "CP949",         "CP949",         FALSE },
    { ENCODING_ISO_2022_KR,  "iso-2022-kr",   "ISO-2022-KR",   "ISO-2022-KR",   FALSE },
    { ENCODING_TIS_620,      "tis-620",       "TIS-620",       "TIS-620",       TRUE  },
};

static const FileEncodingInfo *
file_encoding_get_info(FileEncoding enc)
{
    for (guint i = 0; i < G_N_ELEMENTS(encoding_infos); i++) {
        if (encoding_infos[i].enc == enc) return &encoding_infos[i];
    }
    return NULL;
}

int
file_encoding_get_count(void)
{
    return (int)G_N_ELEMENTS(encoding_infos);
}

FileEncoding
file_encoding_from_id(const char *id)
{
    if (!id) return ENCODING_UTF8;
    for (guint i = 0; i < G_N_ELEMENTS(encoding_infos); i++) {
        if (g_ascii_strcasecmp(encoding_infos[i].id, id) == 0) {
            return encoding_infos[i].enc;
        }
    }

    /* Legacy aliases */
    if (g_ascii_strcasecmp(id, "ascii") == 0) return ENCODING_UTF8;    /* ASCII ⊂ UTF-8 */
    if (g_ascii_strcasecmp(id, "us-ascii") == 0) return ENCODING_UTF8; /* ASCII ⊂ UTF-8 */
    if (g_ascii_strcasecmp(id, "iso8859-1") == 0) return ENCODING_ISO_8859_1;
    if (g_ascii_strcasecmp(id, "iso8859-2") == 0) return ENCODING_ISO_8859_2;
    if (g_ascii_strcasecmp(id, "iso8859-5") == 0) return ENCODING_ISO_8859_5;
    if (g_ascii_strcasecmp(id, "iso8859-6") == 0) return ENCODING_ISO_8859_6;
    if (g_ascii_strcasecmp(id, "iso8859-7") == 0) return ENCODING_ISO_8859_7;
    if (g_ascii_strcasecmp(id, "iso8859-8") == 0) return ENCODING_ISO_8859_8;
    if (g_ascii_strcasecmp(id, "iso8859-9") == 0) return ENCODING_ISO_8859_9;
    if (g_ascii_strcasecmp(id, "iso8859-13") == 0) return ENCODING_ISO_8859_13;
    if (g_ascii_strcasecmp(id, "gb2312") == 0) return ENCODING_GB18030;
    if (g_ascii_strcasecmp(id, "uhc") == 0) return ENCODING_CP949;
    if (g_ascii_strcasecmp(id, "utf-16") == 0) return ENCODING_UTF16LE;

    return ENCODING_UTF8;
}

const char *
file_encoding_to_id(FileEncoding enc)
{
    const FileEncodingInfo *info = file_encoding_get_info(enc);
    return info ? info->id : "utf-8";
}

const char *
file_encoding_to_display_name(FileEncoding enc)
{
    const FileEncodingInfo *info = file_encoding_get_info(enc);
    return info ? info->display : "UTF-8";
}

const char *
file_encoding_to_display_name_from_id(const char *id)
{
    if (!id) return "UTF-8";
    for (guint i = 0; i < G_N_ELEMENTS(encoding_infos); i++) {
        if (g_ascii_strcasecmp(encoding_infos[i].id, id) == 0) {
            return encoding_infos[i].display;
        }
    }
    return id;
}

const char *
file_encoding_to_charset(FileEncoding enc)
{
    const FileEncodingInfo *info = file_encoding_get_info(enc);
    return info ? info->charset : "UTF-8";
}

const char *
file_encoding_get_id_at(int index)
{
    if (index < 0 || (guint)index >= G_N_ELEMENTS(encoding_infos)) return NULL;
    return encoding_infos[index].id;
}

const char *
file_encoding_get_display_name_at(int index)
{
    if (index < 0 || (guint)index >= G_N_ELEMENTS(encoding_infos)) return NULL;
    return encoding_infos[index].display;
}

gboolean
file_encoding_is_utf16(FileEncoding enc)
{
    return (enc == ENCODING_UTF16LE || enc == ENCODING_UTF16BE);
}

gboolean
file_encoding_is_utf32(FileEncoding enc)
{
    return (enc == ENCODING_UTF32LE || enc == ENCODING_UTF32BE);
}

gboolean
file_encoding_is_stream_safe(FileEncoding enc)
{
    const FileEncodingInfo *info = file_encoding_get_info(enc);
    return info ? info->stream_safe : TRUE;
}

/* Map a charset name reported by uchardet to our internal FileEncoding enum.
   uchardet returns IANA/MIME charset names (case-insensitive). */
static FileEncoding
uchardet_charset_to_encoding(const char *charset)
{
    if (!charset || *charset == '\0')
        return ENCODING_UTF8;

    /* ASCII is a strict subset of UTF-8 — treat it as UTF-8 to avoid
       needless conversion that may fail on bytes undefined in CP1252 */
    if (g_ascii_strcasecmp(charset, "ASCII") == 0)         return ENCODING_UTF8;
    if (g_ascii_strcasecmp(charset, "US-ASCII") == 0)      return ENCODING_UTF8;

    /* UTF variants */
    if (g_ascii_strcasecmp(charset, "UTF-8") == 0)         return ENCODING_UTF8;
    if (g_ascii_strcasecmp(charset, "UTF-16") == 0)        return ENCODING_UTF16LE; /* default LE */
    if (g_ascii_strcasecmp(charset, "UTF-16LE") == 0)      return ENCODING_UTF16LE;
    if (g_ascii_strcasecmp(charset, "UTF-16BE") == 0)      return ENCODING_UTF16BE;
    if (g_ascii_strcasecmp(charset, "UTF-32") == 0)        return ENCODING_UTF32LE;
    if (g_ascii_strcasecmp(charset, "UTF-32LE") == 0)      return ENCODING_UTF32LE;
    if (g_ascii_strcasecmp(charset, "UTF-32BE") == 0)      return ENCODING_UTF32BE;

    /* Windows code pages */
    if (g_ascii_strcasecmp(charset, "windows-1250") == 0)  return ENCODING_WINDOWS_1250;
    if (g_ascii_strcasecmp(charset, "windows-1251") == 0)  return ENCODING_WINDOWS_1251;
    if (g_ascii_strcasecmp(charset, "windows-1252") == 0)  return ENCODING_WINDOWS_1252;
    if (g_ascii_strcasecmp(charset, "windows-1253") == 0)  return ENCODING_WINDOWS_1253;
    if (g_ascii_strcasecmp(charset, "windows-1254") == 0)  return ENCODING_WINDOWS_1254;
    if (g_ascii_strcasecmp(charset, "windows-1255") == 0)  return ENCODING_WINDOWS_1255;
    if (g_ascii_strcasecmp(charset, "windows-1256") == 0)  return ENCODING_WINDOWS_1256;
    if (g_ascii_strcasecmp(charset, "windows-1257") == 0)  return ENCODING_WINDOWS_1257;
    if (g_ascii_strcasecmp(charset, "windows-1258") == 0)  return ENCODING_WINDOWS_1258;

    /* ISO 8859 */
    if (g_ascii_strcasecmp(charset, "ISO-8859-1") == 0)    return ENCODING_ISO_8859_1;
    if (g_ascii_strcasecmp(charset, "ISO-8859-2") == 0)    return ENCODING_ISO_8859_2;
    if (g_ascii_strcasecmp(charset, "ISO-8859-5") == 0)    return ENCODING_ISO_8859_5;
    if (g_ascii_strcasecmp(charset, "ISO-8859-6") == 0)    return ENCODING_ISO_8859_6;
    if (g_ascii_strcasecmp(charset, "ISO-8859-7") == 0)    return ENCODING_ISO_8859_7;
    if (g_ascii_strcasecmp(charset, "ISO-8859-8") == 0)    return ENCODING_ISO_8859_8;
    if (g_ascii_strcasecmp(charset, "ISO-8859-9") == 0)    return ENCODING_ISO_8859_9;
    if (g_ascii_strcasecmp(charset, "ISO-8859-13") == 0)   return ENCODING_ISO_8859_13;
    if (g_ascii_strcasecmp(charset, "ISO-8859-15") == 0)   return ENCODING_ISO_8859_15;

    /* CJK */
    if (g_ascii_strcasecmp(charset, "GB18030") == 0)       return ENCODING_GB18030;
    if (g_ascii_strcasecmp(charset, "GBK") == 0)           return ENCODING_GB18030;
    if (g_ascii_strcasecmp(charset, "GB2312") == 0)        return ENCODING_GB18030;
    if (g_ascii_strcasecmp(charset, "Big5") == 0)          return ENCODING_BIG5;
    if (g_ascii_strcasecmp(charset, "EUC-JP") == 0)        return ENCODING_EUC_JP;
    if (g_ascii_strcasecmp(charset, "EUC-KR") == 0)        return ENCODING_EUC_KR;
    if (g_ascii_strcasecmp(charset, "Shift_JIS") == 0 ||
        g_ascii_strcasecmp(charset, "SHIFT-JIS") == 0 ||
        g_ascii_strcasecmp(charset, "SJIS") == 0)          return ENCODING_SHIFT_JIS;
    if (g_ascii_strcasecmp(charset, "ISO-2022-JP") == 0)   return ENCODING_ISO_2022_JP;
    if (g_ascii_strcasecmp(charset, "CP949") == 0 ||
        g_ascii_strcasecmp(charset, "UHC") == 0)           return ENCODING_CP949;

    /* KOI8 */
    if (g_ascii_strcasecmp(charset, "KOI8-R") == 0)        return ENCODING_KOI8_R;
    if (g_ascii_strcasecmp(charset, "KOI8-U") == 0)        return ENCODING_KOI8_U;

    /* Try as a raw charset id in our table */
    return file_encoding_from_id(charset);
}

static void
detect_encoding(const char *data, size_t size, FileEncoding *enc, gboolean *has_bom, size_t *bom_len)
{
    *enc = ENCODING_UTF8;
    *has_bom = FALSE;
    *bom_len = 0;

    /* --- BOM detection (highest confidence) --- */
    if (size >= 4 &&
        (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE &&
        (unsigned char)data[2] == 0x00 && (unsigned char)data[3] == 0x00) {
        *enc = ENCODING_UTF32LE;
        *has_bom = TRUE;
        *bom_len = 4;
        return;
    } else if (size >= 4 &&
               (unsigned char)data[0] == 0x00 && (unsigned char)data[1] == 0x00 &&
               (unsigned char)data[2] == 0xFE && (unsigned char)data[3] == 0xFF) {
        *enc = ENCODING_UTF32BE;
        *has_bom = TRUE;
        *bom_len = 4;
        return;
    } else if (size >= 3 &&
               (unsigned char)data[0] == 0xEF &&
               (unsigned char)data[1] == 0xBB &&
               (unsigned char)data[2] == 0xBF) {
        *enc = ENCODING_UTF8;
        *has_bom = TRUE;
        *bom_len = 3;
        return;
    } else if (size >= 2 &&
               (unsigned char)data[0] == 0xFF &&
               (unsigned char)data[1] == 0xFE) {
        *enc = ENCODING_UTF16LE;
        *has_bom = TRUE;
        *bom_len = 2;
        return;
    } else if (size >= 2 &&
               (unsigned char)data[0] == 0xFE &&
               (unsigned char)data[1] == 0xFF) {
        *enc = ENCODING_UTF16BE;
        *has_bom = TRUE;
        *bom_len = 2;
        return;
    }

    if (size == 0) return;

    /* --- uchardet-based detection (no BOM found) --- */
#ifdef HAVE_UCHARDET
    {
        size_t probe_len = (size > 65536) ? 65536 : size;
        uchardet_t ud = uchardet_new();
        if (ud) {
            int rc = uchardet_handle_data(ud, data, probe_len);
            if (rc == 0) {
                uchardet_data_end(ud);
                const char *charset = uchardet_get_charset(ud);
                if (charset && *charset != '\0') {
                    FileEncoding detected = uchardet_charset_to_encoding(charset);
                    uchardet_delete(ud);
                    *enc = detected;
                    return;
                }
            }
            uchardet_delete(ud);
        }
    }
#endif

    /* --- Fallback: simple null-byte heuristic for UTF-16 without BOM --- */
    {
        size_t check_len = (size > 65536) ? 65536 : size;
        if (size >= 4) {
            size_t le_count = 0, be_count = 0;
            for (size_t i = 0; i + 1 < check_len; i += 2) {
                if (data[i+1] == 0 && data[i] != 0) le_count++;
                if (data[i]   == 0 && data[i+1] != 0) be_count++;
            }
            size_t u32_le = 0, u32_be = 0;
            for (size_t i = 0; i + 3 < check_len; i += 4) {
                if (data[i] != 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 0) u32_le++;
                if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] != 0) u32_be++;
            }
            if      (u32_le > check_len / 16) { *enc = ENCODING_UTF32LE; return; }
            else if (u32_be > check_len / 16) { *enc = ENCODING_UTF32BE; return; }
            else if (le_count > check_len / 4) { *enc = ENCODING_UTF16LE; return; }
            else if (be_count > check_len / 4) { *enc = ENCODING_UTF16BE; return; }
        }
        /* Default: treat as UTF-8 */
        *enc = ENCODING_UTF8;
    }
}

static void
detect_newline_style(const char *data, size_t size, NewlineType *style)
{
    *style = NEWLINE_LF; // Default
    const char *ptr = data;
    size_t check_len = (size > 4096) ? 4096 : size;
    const char *end = data + check_len;
    while (ptr < end) {
        if (*ptr == '\n') {
            *style = NEWLINE_LF;
            return;
        } else if (*ptr == '\r') {
            if (ptr + 1 < end && *(ptr + 1) == '\n') {
                *style = NEWLINE_CRLF;
                return;
            } else {
                *style = NEWLINE_CR;
                return;
            }
        }
        ptr++;
    }
}

static void
detect_newline_style_raw(const char *data, size_t size, FileEncoding enc, NewlineType *style)
{
    *style = NEWLINE_LF; // Default
    size_t check_len = (size > 4096) ? 4096 : size;
    
    if (enc == ENCODING_UTF8) {
        detect_newline_style(data, check_len, style);
        return;
    }

    if (check_len < 2) return;

    if (enc == ENCODING_UTF16LE) {
        /* LE: LF is 0A 00, CR is 0D 00 */
        for (size_t i = 0; i < check_len - 1; i += 2) {
             unsigned char c1 = (unsigned char)data[i];
             unsigned char c2 = (unsigned char)data[i+1];
             
             if (c1 == 0x0A && c2 == 0x00) {
                 *style = NEWLINE_LF;
                 return;
             } else if (c1 == 0x0D && c2 == 0x00) {
                 /* Check for following LF */
                 if (i + 3 < check_len) {
                     unsigned char n1 = (unsigned char)data[i+2];
                     unsigned char n2 = (unsigned char)data[i+3];
                     if (n1 == 0x0A && n2 == 0x00) {
                         *style = NEWLINE_CRLF;
                         return;
                     }
                 }
                 *style = NEWLINE_CR;
                 return;
             }
        }
    } else if (enc == ENCODING_UTF16BE) {
         /* BE: LF is 00 0A, CR is 00 0D */
         for (size_t i = 0; i < check_len - 1; i += 2) {
             unsigned char c1 = (unsigned char)data[i];
             unsigned char c2 = (unsigned char)data[i+1];
             
             if (c1 == 0x00 && c2 == 0x0A) {
                 *style = NEWLINE_LF;
                 return;
             } else if (c1 == 0x00 && c2 == 0x0D) {
                  if (i + 3 < check_len) {
                      unsigned char n1 = (unsigned char)data[i+2];
                      unsigned char n2 = (unsigned char)data[i+3];
                      if (n1 == 0x00 && n2 == 0x0A) {
                          *style = NEWLINE_CRLF;
                          return;
                      }
                  }
                  *style = NEWLINE_CR;
                  return;
             }
         }
    } else if (enc == ENCODING_UTF32LE) {
        /* LE: LF 0A 00 00 00, CR 0D 00 00 00 */
        for (size_t i = 0; i + 3 < check_len; i += 4) {
            if ((unsigned char)data[i] == 0x0A &&
                (unsigned char)data[i+1] == 0x00 &&
                (unsigned char)data[i+2] == 0x00 &&
                (unsigned char)data[i+3] == 0x00) {
                *style = NEWLINE_LF;
                return;
            } else if ((unsigned char)data[i] == 0x0D &&
                       (unsigned char)data[i+1] == 0x00 &&
                       (unsigned char)data[i+2] == 0x00 &&
                       (unsigned char)data[i+3] == 0x00) {
                if (i + 7 < check_len &&
                    (unsigned char)data[i+4] == 0x0A &&
                    (unsigned char)data[i+5] == 0x00 &&
                    (unsigned char)data[i+6] == 0x00 &&
                    (unsigned char)data[i+7] == 0x00) {
                    *style = NEWLINE_CRLF;
                } else {
                    *style = NEWLINE_CR;
                }
                return;
            }
        }
    } else if (enc == ENCODING_UTF32BE) {
        /* BE: LF 00 00 00 0A, CR 00 00 00 0D */
        for (size_t i = 0; i + 3 < check_len; i += 4) {
            if ((unsigned char)data[i] == 0x00 &&
                (unsigned char)data[i+1] == 0x00 &&
                (unsigned char)data[i+2] == 0x00 &&
                (unsigned char)data[i+3] == 0x0A) {
                *style = NEWLINE_LF;
                return;
            } else if ((unsigned char)data[i] == 0x00 &&
                       (unsigned char)data[i+1] == 0x00 &&
                       (unsigned char)data[i+2] == 0x00 &&
                       (unsigned char)data[i+3] == 0x0D) {
                if (i + 7 < check_len &&
                    (unsigned char)data[i+4] == 0x00 &&
                    (unsigned char)data[i+5] == 0x00 &&
                    (unsigned char)data[i+6] == 0x00 &&
                    (unsigned char)data[i+7] == 0x0A) {
                    *style = NEWLINE_CRLF;
                } else {
                    *style = NEWLINE_CR;
                }
                return;
            }
        }
    }
}

/* Robust newline finder that handles \n, \r\n, and \r */
static const char *
find_next_newline(const char *ptr, const char *end, int *nl_len)
{
    if (ptr >= end) return NULL;
    
    size_t len = end - ptr;
    const char *p_lf = (const char *)memchr(ptr, '\n', len);
    
    if (p_lf) {
        /* Check if there's a CR before the LF */
        size_t len_to_lf = p_lf - ptr;
        const char *p_cr = (const char *)memchr(ptr, '\r', len_to_lf);
        
        if (p_cr) {
             /* CR found before LF */
             if (p_cr + 1 < end && p_cr[1] == '\n') {
                 if (nl_len) *nl_len = 2;
             } else {
                 if (nl_len) *nl_len = 1; /* Lone CR */
             }
             return p_cr;
        }
        
        /* No CR before LF. LF is the winner. */
        if (nl_len) *nl_len = 1;
        return p_lf;
    } else {
        /* No LF. Only CR possible. */
        const char *p_cr = (const char *)memchr(ptr, '\r', len);
        if (p_cr) {
             /* Check for CRLF - but we know no LF in this range?
                Wait, if LF was found past 'end'? p_lf is NULL means no LF in [ptr, end).
                So CR cannot be followed by LF within range.
             */
             if (nl_len) *nl_len = 1;
             return p_cr;
        }
    }
    
    return NULL;
}

static size_t
count_newlines(const char *data, size_t len)
{
    size_t count = 0;
    const char *ptr = data;
    const char *end = data + len;
    int nl_len;
    while ((ptr = find_next_newline(ptr, end, &nl_len))) {
        count++;
        ptr += nl_len;
    }
    return count;
}



/* -- Node logic -- */

static PieceNode*
node_new(Piece piece)
{
    PieceNode *n = malloc(sizeof(PieceNode));
    n->piece = piece;
    n->left = n->right = n->parent = NULL;
    n->size_subtree = piece.length;
    n->lf_subtree = 0; /* Caller must set or update */
    return n;
}

static void
update_node(PieceTable *pt, PieceNode *x)
{
    (void)pt; /* Now unused since we use cached_lf */
    if (!x) return;
    x->size_subtree = x->piece.length;
    x->lf_subtree = x->piece.cached_lf;  /* Use cached value instead of scanning */
    
    if (x->left) {
        x->size_subtree += x->left->size_subtree;
        x->lf_subtree += x->left->lf_subtree;
        x->left->parent = x;
    }
    if (x->right) {
        x->size_subtree += x->right->size_subtree;
        x->lf_subtree += x->right->lf_subtree;
        x->right->parent = x;
    }
}

/* Rotate right */
static void
rotate_right(PieceTable *pt, PieceNode *x)
{
    PieceNode *y = x->left;
    if (!y) return;
    
    x->left = y->right;
    if (y->right) y->right->parent = x;
    
    y->parent = x->parent;
    if (!x->parent) pt->root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    
    y->right = x;
    x->parent = y;
    
    update_node(pt, x);
    update_node(pt, y);
}

/* Rotate left */
static void
rotate_left(PieceTable *pt, PieceNode *x)
{
    PieceNode *y = x->right;
    if (!y) return;
    
    x->right = y->left;
    if (y->left) y->left->parent = x;
    
    y->parent = x->parent;
    if (!x->parent) pt->root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    
    y->left = x;
    x->parent = y;
    
    update_node(pt, x);
    update_node(pt, y);
}

static void
splay(PieceTable *pt, PieceNode *x)
{
    while (x->parent) {
        if (!x->parent->parent) {
            if (x->parent->left == x) rotate_right(pt, x->parent);
            else rotate_left(pt, x->parent);
        } else if (x->parent->left == x && x->parent->parent->left == x->parent) {
            rotate_right(pt, x->parent->parent);
            rotate_right(pt, x->parent);
        } else if (x->parent->right == x && x->parent->parent->right == x->parent) {
            rotate_left(pt, x->parent->parent);
            rotate_left(pt, x->parent);
        } else if (x->parent->left == x && x->parent->parent->right == x->parent) {
            /* RL Case */
            rotate_right(pt, x->parent);
            rotate_left(pt, x->parent);
        } else if (x->parent->right == x && x->parent->parent->left == x->parent) {
            /* LR Case */
            rotate_left(pt, x->parent);
            rotate_right(pt, x->parent);
        } else {
             /* Should not happen if all cases covered? 
                Actually, this handles the Zig case (no grandparent) but we check that at top.
                Wait, if we have grandparent, one of the above 4 cases MUST match.
                So this else is unreachable if grandparent exists?
                Top check: if (!x->parent->parent) ...
                So here grandparent exists.
                LL, RR, RL, LR are the only 4 possibilities.
                So we can just put the LR logic here or as else if.
                Safe to leave else as fallback/unreachable or for safety. */
             if (x == x->parent->left) {
                 rotate_right(pt, x->parent);
             } else {
                 rotate_left(pt, x->parent);
             }
        }
    }
}

/* Find node containing offset. Returns cached start_offset of that node too? 
   Nope, we accumulate logic. */
static PieceNode *
find_node_at_offset(PieceTable *pt, size_t offset, size_t *node_start_offset)
{
    PieceNode *curr = pt->root;
    size_t current_start = 0;
    
    while (curr) {
        size_t left_size = curr->left ? curr->left->size_subtree : 0;
        
        if (offset < left_size) {
            curr = curr->left;
        } else if (offset >= left_size + curr->piece.length) {
            offset -= (left_size + curr->piece.length);
            current_start += (left_size + curr->piece.length);
            curr = curr->right;
        } else {
            /* Found */
            if (node_start_offset) *node_start_offset = current_start + left_size;
            splay(pt, curr);
            return curr;
        }
    }
    return NULL;
}

/* Helper: find node by line index.
   Returns the node containing the start of 'line_index'.
   updates 'out_start_byte' to the byte offset of the BEGINNING of this node.
   Wait, identifying which byte starts the line is tricky if a line spans nodes.
   The 'find line' finds the node containing the Nth newline?
   
   If I want "Line 5", I look for 5th newline.
   Actually, "Line 5" starts after 4th newline.
*/
static PieceNode *
find_node_for_line(PieceTable *pt, size_t line_index, size_t *out_node_start_lf, size_t *out_node_start_byte)
{
    PieceNode *curr = pt->root;
    size_t seen_lf = 0;
    size_t seen_byte = 0;

    if (line_index == 0) {
        /* Line 0 starts at the leftmost node */
        curr = pt->root;
        seen_lf = 0;
        seen_byte = 0;
        
        while (curr) {
            if (curr->left) {
                 curr = curr->left;
            } else {
                 if (out_node_start_lf) *out_node_start_lf = seen_lf;
                 if (out_node_start_byte) *out_node_start_byte = seen_byte;
                 splay(pt, curr);
                 return curr;
            }
        }
        return NULL;
    }

    size_t target_lf = line_index - 1;
    size_t target_byte = 0;
    
    curr = pt->root;
    seen_lf = 0;
    seen_byte = 0;
    int found_target = 0;
    
    while (curr) {
        size_t left_lf = curr->left ? curr->left->lf_subtree : 0;
        size_t left_size = curr->left ? curr->left->size_subtree : 0;
        
        if (target_lf < seen_lf + left_lf) {
             curr = curr->left;
        } else {
             size_t node_lf = curr->piece.cached_lf;
             if (target_lf < seen_lf + left_lf + node_lf) {
                 const char *data = get_piece_data(pt, &curr->piece);
                 size_t internal_idx = target_lf - (seen_lf + left_lf);
                 size_t found = 0;
                 const char *ptr = data + curr->piece.start;
                 const char *end = ptr + curr->piece.length;
                 const char *p_ptr = ptr;
                 int nl_len;
                 
                 while (ptr < end && found < internal_idx) {
                     ptr = find_next_newline(ptr, end, &nl_len);
                     ptr += nl_len;
                     found++;
                 }
                 const char *lf_pos = find_next_newline(ptr, end, &nl_len);
                 
                 if (lf_pos) {
                     size_t lf_off = lf_pos - p_ptr;
                     target_byte = seen_byte + left_size + lf_off + nl_len;
                     found_target = 1;
                     break; 
                 }
             }
             seen_lf += left_lf + node_lf;
             seen_byte += left_size + curr->piece.length;
             curr = curr->right;
        }
    }
    
    if (found_target) {
        size_t found_start_byte;
        PieceNode *res = find_node_at_offset(pt, target_byte, &found_start_byte);
        if (res) {
             if (out_node_start_lf) *out_node_start_lf = res->left ? res->left->lf_subtree : 0;
             if (out_node_start_byte) *out_node_start_byte = found_start_byte;
             return res;
        }
    }
    
    return NULL;
}



/* Helper to build balanced tree from array of nodes */
static PieceNode*
build_balanced_tree_recursive(PieceNode **nodes, int start, int end, PieceTable *pt)
{
    if (start > end) return NULL;
    int mid = (start + end) / 2;
    PieceNode *node = nodes[mid];
    
    node->left = build_balanced_tree_recursive(nodes, start, mid - 1, pt);
    if (node->left) node->left->parent = node;
    
    node->right = build_balanced_tree_recursive(nodes, mid + 1, end, pt);
    if (node->right) node->right->parent = node;
    
    update_node(pt, node);
    return node;
}

PieceTable *
piece_table_new(const char *filename)
{
    int fd = -1;
    char *mmap_ptr = NULL;
    size_t size = 0;
    
    /* Only attempt to open if filename is provided */
    if (filename) {
        fd = open(filename, O_RDONLY);
    }
    
    if (fd != -1) {
        struct stat sb;
        fstat(fd, &sb);
        size = sb.st_size;
        if (size > 0)
            mmap_ptr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
    }

    PieceTable *pt = g_new0(PieceTable, 1);
    pt->mmap_base = mmap_ptr;
    pt->mmap_size = size;
    pt->is_mmapped = TRUE;
    pt->orig_data = mmap_ptr;
    pt->orig_size = size;

    if (mmap_ptr == MAP_FAILED) {
        pt->mmap_base = NULL;
        pt->orig_data = NULL;
        pt->orig_size = 0;
        pt->is_mmapped = FALSE;
    }

    size_t bom_len = 0;
    if (pt->orig_data) {
        detect_encoding(pt->orig_data, pt->orig_size, &pt->encoding, &pt->has_bom, &bom_len);
        
        if (pt->encoding != ENCODING_UTF8) {
            /* Disk-backed conversion to UTF-8 for zero-RAM storage. */
            const char *from_codeset = file_encoding_to_charset(pt->encoding);
            
            /* Create temp file for converted UTF-8 data */
            const char *tmp_dir = g_get_tmp_dir();
            if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
            char *temp_template = g_strdup_printf("%s/vite-utf8-XXXXXX", tmp_dir);
            int temp_fd = mkstemp(temp_template);
            if (temp_fd >= 0) {
                unlink(temp_template); /* Unlink immediately for automatic crash cleanup */
            } else {
                g_warning("Failed to create temp file for UTF-16 conversion: %s", strerror(errno));
                /* Fall back to RAM-based conversion */
                g_free(temp_template);
                goto ram_fallback;
            }
            g_free(temp_template);
            
            /* Stream-convert chunks from UTF-16 to UTF-8 */
            const char *src = pt->orig_data + bom_len;
            size_t src_remaining = pt->orig_size - bom_len;
            size_t chunk_size = UTF16_CONVERT_CHUNK_SIZE;
            size_t total_written = 0;


            
            gint64 work_budget_us = 0;
            while (src_remaining > 0) {
                gint64 chunk_start = g_get_monotonic_time();
                size_t to_convert = (src_remaining < chunk_size) ? src_remaining : chunk_size;
                if (file_encoding_is_utf16(pt->encoding)) {
                    /* Keep UTF-16 code units aligned. */
                    if (to_convert < src_remaining && (to_convert % 2) != 0) {
                        to_convert--;
                    }
                } else if (file_encoding_is_utf32(pt->encoding)) {
                    /* Keep UTF-32 code units aligned. */
                    if (to_convert < src_remaining && (to_convert % 4) != 0) {
                        to_convert -= (to_convert % 4);
                    }
                }
                if (to_convert == 0) {
                    close(temp_fd);
                    goto ram_fallback;
                }
                
                gsize bytes_read, bytes_written;
                GError *conv_error = NULL;
                char *utf8_chunk = g_convert(src, to_convert, "UTF-8", from_codeset,
                                             &bytes_read, &bytes_written, &conv_error);
                
                if (!utf8_chunk) {
                    g_warning("UTF-16 conversion failed: %s", conv_error ? conv_error->message : "Unknown");
                    g_clear_error(&conv_error);
                    close(temp_fd);
                    goto ram_fallback;
                }
                
                /* Write to temp file */
                ssize_t written = write(temp_fd, utf8_chunk, bytes_written);
                g_free(utf8_chunk);
                
                if (written != (ssize_t)bytes_written) {
                    g_warning("Failed to write to temp file: %s", strerror(errno));
                    close(temp_fd);
                    goto ram_fallback;
                }
                
                total_written += bytes_written;
                src += bytes_read;
                src_remaining -= bytes_read;
                work_budget_us += (g_get_monotonic_time() - chunk_start);
                utf16_convert_throttle(pt->orig_size, &work_budget_us);
            }
            
            /* Sync and get file size */
            /* Munmap original UTF-16 file - no longer needed */
            if (pt->mmap_base && pt->mmap_size > 0) {
                munmap(pt->mmap_base, pt->mmap_size);
            }
            pt->mmap_base = NULL;
            pt->mmap_size = 0;
            
            /* mmap the temp file */
            if (total_written > 0) {
                char *utf8_mmap = mmap(NULL, total_written, PROT_READ, MAP_PRIVATE, temp_fd, 0);
                close(temp_fd);
                
                if (utf8_mmap == MAP_FAILED) {
                    g_warning("Failed to mmap temp file: %s", strerror(errno));
                    pt->orig_data = g_strdup("");
                    pt->orig_size = 0;
                    pt->is_mmapped = FALSE;
                } else {
                    pt->mmap_base = utf8_mmap;
                    pt->mmap_size = total_written;
                    pt->orig_data = utf8_mmap;
                    pt->orig_size = total_written;
                    pt->is_mmapped = TRUE;
                }
            } else {
                close(temp_fd);
                pt->orig_data = NULL;
                pt->orig_size = 0;
                pt->is_mmapped = FALSE;
            }
        } else if (bom_len > 0) {
            /* UTF-8 with BOM - just skip the BOM bytes */
            pt->orig_data += bom_len;
            pt->orig_size -= bom_len;
        }
        /* UTF-8 without BOM: no changes needed, use mmap directly */
        
        detect_newline_style(pt->orig_data, pt->orig_size, &pt->newline_style);
    }

    /* Skip to tree building - non-UTF8 conversion may use ram_fallback */
    goto build_tree;
        
ram_fallback:
    /* RAM-based fallback for when temp file conversion fails */
    if (pt->encoding != ENCODING_UTF8) {
        const char *from_codeset = file_encoding_to_charset(pt->encoding);
        gsize bytes_read, bytes_written;
        GError *error = NULL;
        char *utf8_data = g_convert(pt->orig_data + bom_len, pt->orig_size - bom_len, 
                                     "UTF-8", from_codeset, &bytes_read, &bytes_written, &error);
        if (utf8_data) {
            /* Munmap original since we're using RAM now */
            if (pt->mmap_base && pt->mmap_size > 0) {
                munmap(pt->mmap_base, pt->mmap_size);
            }
            pt->mmap_base = NULL;
            pt->mmap_size = 0;
            pt->orig_data = utf8_data;
            pt->orig_size = bytes_written;
            pt->is_mmapped = FALSE;
        } else {
            g_warning("Failed to convert file: %s", error->message);
            g_clear_error(&error);
            pt->orig_data = g_strdup("");
            pt->orig_size = 0;
            pt->is_mmapped = FALSE;
        }
        detect_newline_style(pt->orig_data, pt->orig_size, &pt->newline_style);
    }
        
build_tree:

    pt->add_buffer = disk_buffer_new();
    pt->external_sources = g_ptr_array_new_full(0, g_free); /* We need custom free func, see piece_table_free */
    
    if (pt->orig_size > 0) {
        /* Chunking strategy */
        size_t chunk_size = 16 * 1024; /* 16KB */
        size_t count = (pt->orig_size + chunk_size - 1) / chunk_size;
        
        PieceNode **nodes = malloc(count * sizeof(PieceNode*));
        
        for (size_t i = 0; i < count; i++) {
            size_t start = i * chunk_size;
            size_t len = chunk_size;
            if (start + len > pt->orig_size) len = pt->orig_size - start;
            
            Piece p = { SOURCE_ORIGINAL, start, len, count_newlines(pt->orig_data + start, len) };
            nodes[i] = node_new(p);
        }
        
        pt->root = build_balanced_tree_recursive(nodes, 0, (int)count - 1, pt);
        free(nodes);
    } else {
        pt->root = NULL;
    }
    return pt;
}

static void
free_tree(PieceNode *node)
{
    if (!node) return;
    free_tree(node->left);
    free_tree(node->right);
    free(node);
}

static void
piece_table_clear_external_sources(PieceTable *pt)
{
    if (!pt || !pt->external_sources) return;

    for (guint i = 0; i < pt->external_sources->len; i++) {
        PieceTableSource *src = g_ptr_array_index(pt->external_sources, i);
        if (!src) continue;
        if (src->mmap_base && src->size > 0) {
            munmap(src->mmap_base, src->size);
        }
        if (src->fd >= 0) close(src->fd);
        g_free(src->path);
        g_free(src);
    }

    /* Elements are already freed above. */
    g_ptr_array_free(pt->external_sources, FALSE);
    pt->external_sources = NULL;
}

void
piece_table_free(PieceTable *pt)
{
    /* Free all tree nodes recursively */
    free_tree(pt->root);
    if (pt->is_mmapped) {
        if (pt->mmap_base && pt->mmap_size > 0) munmap(pt->mmap_base, pt->mmap_size);
    } else {
        g_free(pt->orig_data);
    }
    
    disk_buffer_free(pt->add_buffer);
    
    /* Clean up external sources */
    piece_table_clear_external_sources(pt);
    
    free(pt);
}

size_t
piece_table_get_length(PieceTable *pt)
{
    return pt->root ? pt->root->size_subtree : 0;
}

size_t
piece_table_get_line_count(PieceTable *pt)
{
    /* Always at least 1 line */
    if (!pt->root) return 1;
    size_t n = pt->root->lf_subtree;
    /* If the last character is a newline, we have N+1 lines?
       Common editor logic: "a\n" is 2 lines. "a" is 1 line.
       count(new lines) = 1 in "a\n".
       So lines = newlines + 1.
    */
    return n + 1;
}

/* Insert logic */
void
piece_table_insert(PieceTable *pt, size_t offset, const char *text, size_t len)
{
    if (len == 0) return;
    
    /* Validate length */
    if (!resource_size_valid(len)) {
         g_warning("piece_table_insert: Invalid length %zu (overflow)", len);
         return;
    }
    /* pt->change_count++ handled in insert_piece_at_offset or we do it here?
       insert_piece_at_offset increments it.
       But if we chunk, we increment 1100 times? 
       That's fine, change_count is just a revision ID.
    */
    
    /* Add text to buffer */
    size_t start_in_add = pt->add_buffer->len;
    disk_buffer_append(pt->add_buffer, text, len);
    
    /* Chunking strategy */
    size_t chunk_size = 64 * 1024;
    size_t current_off = 0;
    
    /* If len is small, just do one insert */
    if (len <= chunk_size) {
        size_t lf_count = count_newlines(text, len);
        Piece new_piece = { SOURCE_ADD, start_in_add, len, lf_count };
        insert_piece_at_offset(pt, offset, new_piece);
        return;
    }

    while (current_off < len) {
        size_t chunk = chunk_size;
        if (current_off + chunk > len) chunk = len - current_off;
        
        size_t chunk_lf = count_newlines(text + current_off, chunk);
        Piece p = { SOURCE_ADD, start_in_add + current_off, chunk, chunk_lf };
        
        insert_piece_at_offset(pt, offset + current_off, p);
        
        current_off += chunk;
    }
}



/* Optimized bulk replacement: replaces entire content with new content.
 * Creates chunked pieces like piece_table_new to maintain O(log N) line access.
 * The lf_count parameter is ignored - we count per-chunk for proper tree balance.
 */
void
piece_table_replace_all(PieceTable *pt, const char *new_content, size_t len, size_t lf_count)
{
    (void)lf_count; /* Not used - we count per chunk */
    
    pt->change_count++;
    
    free_tree(pt->root);
    pt->root = NULL;
    
    if (len == 0) return;
    
    /* Add text to buffer */
    size_t start_in_add = pt->add_buffer->len;
    disk_buffer_append(pt->add_buffer, new_content, len);
    
    /* Chunking strategy like piece_table_new - 16KB chunks for O(log N) line access */
    size_t chunk_size = 16 * 1024;
    size_t count = (len + chunk_size - 1) / chunk_size;
    
    PieceNode **nodes = malloc(count * sizeof(PieceNode*));
    
    for (size_t i = 0; i < count; i++) {
        size_t chunk_start = i * chunk_size;
        size_t chunk_len = chunk_size;
        if (chunk_start + chunk_len > len) chunk_len = len - chunk_start;
        
        /* Count newlines in this chunk */
        size_t chunk_lf = 0;
        const char *chunk_data = new_content + chunk_start;
        for (size_t j = 0; j < chunk_len; j++) {
            if (chunk_data[j] == '\n') chunk_lf++;
        }
        
        Piece p = { SOURCE_ADD, start_in_add + chunk_start, chunk_len, chunk_lf };
        nodes[i] = node_new(p);
    }
    
    pt->root = build_balanced_tree_recursive(nodes, 0, (int)count - 1, pt);
    free(nodes);
}

void
piece_table_replace_from_fd(PieceTable *pt, int fd, size_t len, size_t lf_count)
{
    (void)lf_count; /* Not used - we count per chunk */
    
    /* 1. Map the new file */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        g_warning("fstat failed: %s", strerror(errno));
        return;
    }
    size_t size = st.st_size;
    if (size == 0) size = 1; /* Handle empty file case for mmap? */
    
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        g_warning("mmap failed: %s", strerror(errno));
        return;
    }

    pt->change_count++;
    
    /* 2. Free old tree */
    free_tree(pt->root);
    pt->root = NULL;

    /* 3. Free old backing store */
    if (pt->is_mmapped) {
        if (pt->mmap_base && pt->mmap_size > 0)
            munmap(pt->mmap_base, pt->mmap_size);
    } else {
        if (pt->orig_data)
            g_free(pt->orig_data);
    }
    
    /* 4. Set up new backing store */
    pt->is_mmapped = TRUE;
    pt->mmap_base = map;
    pt->mmap_size = size;
    pt->orig_data = map;
    pt->orig_size = len; /* Use passed length (should match file size mostly) */
    
    if (len > size) pt->orig_size = size; /* Safety cap */
    
    /* 5. Reset add buffer */
    pt->add_buffer->len = 0;
    
    /* 6. Rebuild tree (Chunking strategy for O(log N) access) */
    if (len == 0) return;
    
    size_t chunk_size = 16 * 1024;
    size_t count = (len + chunk_size - 1) / chunk_size;
    
    PieceNode **nodes = malloc(count * sizeof(PieceNode*));
    if (!nodes) return;
    
    for (size_t i = 0; i < count; i++) {
        size_t chunk_start = i * chunk_size;
        size_t chunk_len = chunk_size;
        if (chunk_start + chunk_len > len) chunk_len = len - chunk_start;
        
        /* Count newlines in this chunk */
        size_t chunk_lf = 0;
        const char *chunk_data = pt->orig_data + chunk_start;
        for (size_t j = 0; j < chunk_len; j++) {
            if (chunk_data[j] == '\n') chunk_lf++;
        }
        
        Piece p = { SOURCE_ORIGINAL, chunk_start, chunk_len, chunk_lf };
        nodes[i] = node_new(p);
    }
    
    pt->root = build_balanced_tree_recursive(nodes, 0, (int)count - 1, pt);
    free(nodes);
}

/* Async versions */
struct _PieceTableReplaceTask {
    PieceTable *pt;
    int fd;
    size_t len;
    
    char *map;
    size_t map_size;
    
    size_t chunk_size;
    size_t total_chunks;
    size_t current_chunk;
    
    PieceNode **nodes;
};

PieceTableReplaceTask *
piece_table_replace_async_start(PieceTable *pt, int fd, size_t len)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return NULL;
    
    size_t size = st.st_size;
    if (size == 0) return NULL; /* Async empty replace not supported for now */
    
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) return NULL;
    
    PieceTableReplaceTask *task = g_new0(PieceTableReplaceTask, 1);
    task->pt = pt;
    task->fd = fd;
    task->len = len;
    task->map = map;
    task->map_size = size;
    
    task->chunk_size = 16 * 1024;
    task->total_chunks = (len + task->chunk_size - 1) / task->chunk_size;
    task->current_chunk = 0;
    task->nodes = g_malloc0(task->total_chunks * sizeof(PieceNode*));
    
    return task;
}

gboolean
piece_table_replace_async_step(PieceTableReplaceTask *task, gint64 budget_us, double *progress_out)
{
    gint64 start_time = g_get_monotonic_time();
    
    while (task->current_chunk < task->total_chunks) {
        size_t i = task->current_chunk;
        size_t chunk_start = i * task->chunk_size;
        size_t chunk_len = task->chunk_size;
        if (chunk_start + chunk_len > task->len) chunk_len = task->len - chunk_start;
        
        /* Count newlines in this chunk */
        size_t chunk_lf = 0;
        const char *chunk_data = task->map + chunk_start;
        for (size_t j = 0; j < chunk_len; j++) {
            if (chunk_data[j] == '\n') chunk_lf++;
        }
        
        Piece p = { SOURCE_ORIGINAL, chunk_start, chunk_len, chunk_lf };
        task->nodes[i] = node_new(p);
        
        task->current_chunk++;
        
        /* Check budget every 20 chunks */
        if (task->current_chunk % 20 == 0) {
            if (g_get_monotonic_time() - start_time > budget_us) break;
        }
    }
    
    if (progress_out) {
        *progress_out = (double)task->current_chunk / task->total_chunks;
    }
    
    return (task->current_chunk >= task->total_chunks);
}

void
piece_table_replace_async_finalize(PieceTableReplaceTask *task)
{
    PieceTable *pt = task->pt;
    
    pt->change_count++;
    
    /* 1. Free old tree */
    free_tree(pt->root);
    pt->root = NULL;

    /* 2. Free old backing store */
    if (pt->is_mmapped) {
        if (pt->mmap_base && pt->mmap_size > 0)
            munmap(pt->mmap_base, pt->mmap_size);
    } else {
        if (pt->orig_data)
            g_free(pt->orig_data);
    }
    
    /* 3. Set up new backing store */
    pt->is_mmapped = TRUE;
    pt->mmap_base = task->map;
    pt->mmap_size = task->map_size;
    pt->orig_data = task->map;
    pt->orig_size = task->len;
    
    if (task->len > task->map_size) pt->orig_size = task->map_size;
    
    /* 4. Reset add buffer */
    pt->add_buffer->len = 0;
    
    /* 5. Finalize tree build */
    pt->root = build_balanced_tree_recursive(task->nodes, 0, (int)task->total_chunks - 1, pt);
    
    /* Free task container (but map belongs to pt now) */
    g_free(task->nodes);
    g_free(task);
}

void
piece_table_replace_async_cancel(PieceTableReplaceTask *task)
{
    if (!task) return;
    
    /* Free created nodes */
    for (size_t i = 0; i < task->current_chunk; i++) {
        if (task->nodes[i]) free(task->nodes[i]);
    }
    
    munmap(task->map, task->map_size);
    g_free(task->nodes);
    g_free(task);
}


/* Delete logic */
/* Helper: Split buffer at logical offset. 
   Returns the node that ends exactly at offset (left part), 
   or the node that starts exactly at offset?
   Splay approach: After splay(find(offset)), root contains offset.
   If offset is middle of root, we split root.
   If offset is 0 of root, root is Right, Left is root->left...
*/

static void
ensure_split_at(PieceTable *pt, size_t offset)
{
    /* Make sure there is a boundary at 'offset' */
    size_t node_start;
    PieceNode *node = find_node_at_offset(pt, offset, &node_start);
    if (!node) return; /* End of file or empty */
    
    size_t local_off = offset - node_start;
    if (local_off == 0 || local_off == node->piece.length) return;
    
    /* Split 'node' into A and B */
    /* Reuse logic from insert:
       node becomes A (0..local_off)
       Create B (local_off..len)
       A < B
    */
    Piece right_piece = node->piece;
    right_piece.start += local_off;
    right_piece.length -= local_off;
    /* Calculate and cache LF for right piece */
    const char *src_data = (right_piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->mmap_base;
    right_piece.cached_lf = count_newlines(src_data + right_piece.start, right_piece.length);
    PieceNode *right_node = node_new(right_piece);
    
    /* Update Left (node) - use subtraction optimization */
    size_t old_total_lf = node->piece.cached_lf;
    node->piece.length = local_off;
    node->piece.cached_lf = old_total_lf - right_piece.cached_lf;
    node->size_subtree = local_off; // Temp
    
    /* Insert B right of A */
    /* A is root. A->right becomes B->right. A->right is B. */
    
    PieceNode *old_right = node->right;
    
    node->right = right_node;
    right_node->parent = node;
    
    right_node->right = old_right;
    if (old_right) old_right->parent = right_node;
    
    update_node(pt, right_node);
    update_node(pt, node);
}

void
piece_table_delete(PieceTable *pt, size_t offset, size_t len)
{
    if (len == 0 || !pt->root) return;
    
    if (!resource_size_valid(len)) {
        g_warning("piece_table_delete: Invalid length %zu (overflow)", len);
        return;
    }
    
    pt->change_count++;
    
    size_t total = piece_table_get_length(pt);
    if (offset + len > total) len = total - offset;

    /* Strategy:
       Split at offset.
       Split at offset + len.
       Then we have: [LeftPart] [RangeToDelete] [RightPart]
       We can remove [RangeToDelete] subtree.
    */
    
    ensure_split_at(pt, offset);
    ensure_split_at(pt, offset + len);
    
    /* 1. Splay node at 'offset-1' or just 'offset'?
       If we splay 'offset', it brings the node STARTING at offset to root.
       Then everything >= offset is in Root+Right.
    */
    
    size_t start_node_off;
    PieceNode *start_node = find_node_at_offset(pt, offset, &start_node_off);
    /* start_node should now START at offset because of ensure_split_at(offset) 
       Wait, find_node returns the node containing the offset.
       If offset is boundary, it might return the one before or after depending on logic.
       My logic: offset < left_size -> left.
       >= -> right.
       If offset == 0 of node, it returns that node.
       So yes, start_node starts at offset.
    */
    
    if (!start_node) return; /* Should not happen */
    
    /* Splay start_node. Now Root = start_node. 
       Everything < offset is in Root->Left.
       Everything >= offset is Root + Root->Right.
    */
    
    /* Now find end boundary */
    /* We want to cut from Offset to Offset+Len. 
       We already split at Offset+Len.
       So the end of deletion is a boundary.
       The node Starting at Offset+Len should be preserved.
    */
    
    size_t end_node_off;
    PieceNode *end_node = find_node_at_offset(pt, offset + len, &end_node_off);
    
    if (!end_node) {
        /* Deleting until EOF */
        /* Root is start_node. Root->Left is preserved.
           Root and Root->Right are deleted?
           Yes.
        */
        PieceNode *left_preserved = start_node->left;
        if (left_preserved) {
            left_preserved->parent = NULL;
            pt->root = left_preserved;
        } else {
            pt->root = NULL;
        }
        start_node->left = NULL; /* Detach preserved left branch */
        free_tree(start_node);
        return;
    }
    
    /* We have end_node. Splay it.
       Now Root = end_node. (Starts at offset+len).
       Root->Left contains the Stuff to Delete AND the Stuff Before Offset.
    */
    
    /* This splay messes up the first splay structure.
       Standard Splay Delete Range (start, end):
       1. Splay(start-1) -> Root.
       2. Splay(end+1) -> Root->Right.
       3. Root->Right->Left is the range.
    */
    
    /* Case 1: Deleting from 0? */
    PieceNode *left_anchor = NULL;
    if (offset > 0) {
        size_t off;
        left_anchor = find_node_at_offset(pt, offset - 1, &off);
        /* because we split at offset, offset-1 must be in the node Left of boundary. */
    }
    
    if (left_anchor) {
        splay(pt, left_anchor);
        /* Root is left_anchor. Root->right contains range + tail. */
        /* Now splay end_node (starts at offset + len) INSIDE Root->right?
           Splaying normally Bubbles it to Root.
        */
        splay(pt, end_node);
        /* Now Root = end_node. 
           Root->Left contains everything before end.
           Root->Left->Right ... this is getting complicated.
        */
        /* Let's try simpler:
           Splay(left_anchor). Root = left_anchor.
           Expose Right child.
           The node `end_node` is in Right child.
           Splay `end_node`. Root = end_node.
           left_anchor is now Left child of `end_node` (if it's the max of left part).
           Actually left_anchor is < end_node.
           
           If we just splay `end_node`, `left_anchor` is somewhere in `end_node->left`.
           If we then splay `left_anchor` but STOP when parent is `end_node`... (Standard Splay Range method).
           
           Since I don't have `splay_to_root_child`, let's hack:
           
           Splay(end_node).
           Cut end_node->left. Call it L.
           Splay(left_anchor) in L.
             (Hack: Temporarily make L a root, splay, reattach).
           L_new->right is the Range! Delete it!
        */
        
        splay(pt, end_node);
        PieceNode *L = end_node->left;
        if (L) L->parent = NULL;
        
        pt->root = L;
        splay(pt, left_anchor); /* Now left_anchor is new root of L */
        
        /* content to delete is left_anchor->right */
        PieceNode *deleted = left_anchor->right;
        left_anchor->right = NULL;
        free_tree(deleted);
        update_node(pt, left_anchor);
        
        /* Reattach */
        end_node->left = left_anchor;
        left_anchor->parent = end_node;
        pt->root = end_node;
        update_node(pt, end_node);
        
    } else {
        /* Deleting from 0 */
        /* Splay end_node. Root = end_node.
           Root->Left is the range [0...len].
           Delete Root->Left.
        */
        splay(pt, end_node);
        PieceNode *deleted = end_node->left;
        end_node->left = NULL;
        free_tree(deleted);
        update_node(pt, end_node);
    }
}

/* Helper to get text from range */
char *
piece_table_get_text_range(PieceTable *pt, size_t offset, size_t len)
{
    if (len == 0) return g_strdup("");
    
    if (!resource_size_valid(len)) {
        g_warning("piece_table_get_text_range: Invalid size %zu", len);
        return NULL;
    }
    
    if (!resource_can_allocate(len + 1)) {
        g_warning("piece_table_get_text_range: Cannot allocate %zu bytes", len + 1);
        return NULL;
    }

    GString *res = resource_safe_g_string_sized_new(len);
    if (!res) return NULL;
    
    /* Naive: iterate */
    /* Or split/find nodes. iterating chars is slow. 
       We should iterate pieces. 
    */
    
    size_t cur = offset;
    size_t remaining = len;
    
    while (remaining > 0) {
        size_t node_start;
        PieceNode *n = find_node_at_offset(pt, cur, &node_start);
        if (!n) break;
        
        size_t off_in_node = cur - node_start;
        size_t avail = n->piece.length - off_in_node;
        size_t chunk = (avail < remaining) ? avail : remaining;
        
        const char *data = get_piece_data(pt, &n->piece);
        g_string_append_len(res, data + n->piece.start + off_in_node, chunk);
        
        cur += chunk;
        remaining -= chunk;
    }
    
    return g_string_free(res, FALSE);
}

/* Walk the tree in-order starting from a node to build the line */
/* This is O(K) where K is number of pieces in the line. */
char *
piece_table_get_line(PieceTable *pt, size_t line_index, size_t *out_len)
{
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    
    if (!node) {
        *out_len = 0;
        return g_strdup("");
    }
    
    size_t relative_lf = line_index - start_lf;
    const char *data = get_piece_data(pt, &node->piece);
    data += node->piece.start;
    size_t len = node->piece.length;
    
    size_t internal_offset = 0;
    int nl_len;
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = data;
        const char *end = data + len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - data;
    }
    
    GString *res = g_string_new("");
    const char *ptr = data + internal_offset;
    const char *node_end = data + len;
    
    const char *eol = find_next_newline(ptr, node_end, &nl_len);
    if (eol) {
        g_string_append_len(res, ptr, eol - ptr + nl_len); 
        *out_len = res->len;
        return g_string_free(res, FALSE);
    }
    
    g_string_append_len(res, ptr, node_end - ptr);
    
    PieceNode *curr = node;
    while (1) {
        if (curr->right) {
            curr = curr->right;
            while (curr->left) curr = curr->left;
        } else {
            PieceNode *p = curr->parent;
            while (p && curr == p->right) {
                curr = p;
                p = p->parent;
            }
            curr = p;
        }
        
        if (!curr) break; 
        
        const char *cdata = get_piece_data(pt, &curr->piece);
        cdata += curr->piece.start;
        size_t clen = curr->piece.length;
        const char *cend = cdata + clen;
        
        const char *ceol = find_next_newline(cdata, cend, &nl_len);
        if (ceol) {
            g_string_append_len(res, cdata, ceol - cdata + nl_len);
            break;
        } else {
            g_string_append_len(res, cdata, clen);
        }
    }
    
    *out_len = res->len;
    return g_string_free(res, FALSE);
}

/* Zero-allocation (if fits) line retrieval.
   Returns the total length of the line.
   If result <= buf_len, then 'buf' contains the full line (null-terminated if space allows, but usually we handle length).
   Actually, caller should know length.
   We will treat buf_len as capacity. We write up to capacity.
   Return value is the TRUE length of the line.
   If return > buf_len, truncation occurred. 
*/
size_t
piece_table_get_line_into(PieceTable *pt, size_t line_index, char *buf, size_t buf_len)
{
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    
    if (!node) return 0;
    
    size_t current_len = 0;
    
    size_t relative_lf = line_index - start_lf;
    const char *data = get_piece_data(pt, &node->piece);
    data += node->piece.start;
    size_t len = node->piece.length;
    
    size_t internal_offset = 0;
    int nl_len;
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = data;
        const char *end = data + len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - data;
    }
    
    const char *ptr = data + internal_offset;
    const char *node_end = data + len;
    
    const char *eol = find_next_newline(ptr, node_end, &nl_len);
    if (eol) {
        size_t seg_len = eol - ptr + nl_len;
        if (current_len < buf_len) {
            size_t copy = seg_len;
            if (current_len + copy > buf_len) copy = buf_len - current_len;
            memcpy(buf + current_len, ptr, copy);
        }
        return seg_len;
    }
    
    size_t seg_len = node_end - ptr;
    if (current_len < buf_len) {
        size_t copy = seg_len;
        if (current_len + copy > buf_len) copy = buf_len - current_len;
        memcpy(buf + current_len, ptr, copy);
    }
    current_len += seg_len;
    
    PieceNode *curr = node;
    while (1) {
        if (curr->right) {
            curr = curr->right;
            while (curr->left) curr = curr->left;
        } else {
            PieceNode *p = curr->parent;
            while (p && curr == p->right) {
                curr = p;
                p = p->parent;
            }
            curr = p;
        }
        
        if (!curr) break; 
        
        const char *cdata = get_piece_data(pt, &curr->piece);
        cdata += curr->piece.start;
        size_t clen = curr->piece.length;
        const char *cend = cdata + clen;
        
        const char *ceol = find_next_newline(cdata, cend, &nl_len);
        if (ceol) {
            size_t sub = ceol - cdata + nl_len;
            if (current_len < buf_len) {
                size_t copy = sub;
                if (current_len + copy > buf_len) copy = buf_len - current_len;
                memcpy(buf + current_len, cdata, copy);
            }
            current_len += sub;
            break;
        } else {
            if (current_len < buf_len) {
                size_t copy = clen;
                if (current_len + copy > buf_len) copy = buf_len - current_len;
                memcpy(buf + current_len, cdata, copy);
            }
            current_len += clen;
        }
    }
    
    return current_len;
}

size_t
piece_table_get_line_length(PieceTable *pt, size_t line_index)
{
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    
    if (!node) return 0;
    
    size_t relative_lf = line_index - start_lf;
    const char *data = get_piece_data(pt, &node->piece);
    data += node->piece.start;
    size_t len = node->piece.length;
    
    size_t internal_offset = 0;
    int nl_len;
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = data;
        const char *end = data + len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - data;
    }
    
    size_t total_len = 0;
    const char *ptr = data + internal_offset;
    const char *node_end = data + len;
    
    const char *eol = find_next_newline(ptr, node_end, &nl_len);
    if (eol) {
        return (eol - ptr + nl_len);
    }
    total_len += (node_end - ptr);
    
    PieceNode *curr = node;
    while (1) {
        if (curr->right) {
            curr = curr->right;
            while (curr->left) curr = curr->left;
        } else {
            PieceNode *p = curr->parent;
            while (p && curr == p->right) {
                curr = p;
                p = p->parent;
            }
            curr = p;
        }
        
        if (!curr) break;
        
        const char *cdata = get_piece_data(pt, &curr->piece);
        cdata += curr->piece.start;
        size_t clen = curr->piece.length;
        const char *cend = cdata + clen;
        
        const char *ceol = find_next_newline(cdata, cend, &nl_len);
        if (ceol) {
            total_len += (ceol - cdata + nl_len);
            break;
        } else {
            total_len += clen;
        }
    }
    
    return total_len;
}

size_t
piece_table_get_line_of_offset(PieceTable *pt, size_t offset)
{
    /* Find node containing offset, summing LF of left subtrees + LF inside node up to split */
    size_t node_start;
    PieceNode *node = find_node_at_offset(pt, offset, &node_start);
    if (!node) {
        if (pt->root && offset >= pt->root->size_subtree)
            return pt->root->lf_subtree;
        return 0;
    }
    
    /* We need the path to sum Left subtrees.
       Splay implementation moves node to root.
       So Root->Left contains all preceding content.
       So line index = Root->Left->lf_subtree + LF inside Root before offset.
    */
    size_t lines_before = node->left ? node->left->lf_subtree : 0;
    
    /* Count newlines in node up to (offset - node_start) */
    size_t local_off = offset - node_start;
    const char *data = get_piece_data(pt, &node->piece);
    size_t local_lf = count_newlines(data + node->piece.start, local_off);
    
    return lines_before + local_lf;
}

size_t
piece_table_get_offset_of_line(PieceTable *pt, size_t line_index)
{
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    if (!node) {
        if (pt->root && line_index >= pt->root->lf_subtree)
            return pt->root->size_subtree;
        return 0;
    }
    
    /* node is root. node->left lines = start_lf.
       We want line_index.
       Lines to skip in node = line_index - start_lf.
       We need to find byte offset of Nth newline.
       If N=0, we are at start of node (start_byte).
       If N=1, we are after 1st newline.
    */
    
    size_t relative_lf = line_index - start_lf;
    size_t internal_offset = 0;
    
    const char *data = get_piece_data(pt, &node->piece);
    /* Use direct pointer math */
    
    size_t len = node->piece.length;
    const char *p_data = data + node->piece.start;
    
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = p_data;
        const char *end = p_data + len;
        int nl_len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - p_data;
    }
    
    return start_byte + internal_offset;
}

/* -- Traversal -- */

static void
traverse_node_for_lines(PieceTable *pt, PieceNode *node, void (*func)(size_t len, void *user_data), void *user_data, size_t *acc_len)
{
    if (!node) return;
    
    traverse_node_for_lines(pt, node->left, func, user_data, acc_len);
    
    /* Process current piece */
    const char *data = get_piece_data(pt, &node->piece);
    const char *ptr = data + node->piece.start;
    const char *end = ptr + node->piece.length;
    
    const char *scan = ptr;
    int nl_len;
    while (scan < end) {
        const char *nl = find_next_newline(scan, end, &nl_len);
        if (nl) {
            size_t seg_len = nl - scan;
            size_t full_len = *acc_len + seg_len + nl_len;
            
            func(full_len, user_data);
            
            *acc_len = 0;
            scan = nl + nl_len;
        } else {
            *acc_len += (end - scan);
            break;
        }
    }
    
    traverse_node_for_lines(pt, node->right, func, user_data, acc_len);
}

void
piece_table_foreach_line(PieceTable *pt, void (*func)(size_t line_len, void *user_data), void *user_data)
{
    if (!pt || !pt->root) return;
    
    size_t acc_len = 0;
    traverse_node_for_lines(pt, pt->root, func, user_data, &acc_len);
    
    /* If there is leftover text (file doesn't end in newline), emit it as final line */
    if (acc_len > 0) {
        func(acc_len, user_data);
    }
    /* Note: If file ends in newline, we don't emit an empty line here to avoid confusing line counting
       unless the editor logic specifically expects it. 
       Usually "A\n" -> 2 lines. 
       If we iterate, we emit 1 for "A\n". The second line is empty and implicit.
       Our loop in editor_widget expects total_lines.
       If total_lines > emitted_lines, we know the last one is empty. */
}

/* -- Iterator Implementation -- */

void
piece_table_iter_init(PieceTable *pt, PieceTableIter *iter)
{
    if (!pt || !iter) return;
    memset(iter, 0, sizeof(PieceTableIter));
    iter->pt = pt;
    
    /* Start at leftmost node of root */
    PieceNode *curr = pt->root;
    while (curr && curr->left) {
        curr = curr->left;
    }
    iter->current_node = curr;
    iter->offset_in_node = 0;
    iter->current_line_index = 0;
}

static PieceNode *
node_next_in_order(PieceNode *node)
{
    if (!node) return NULL;
    
    /* If has right child, go right, then leftmost */
    if (node->right) {
        node = node->right;
        while (node->left) node = node->left;
        return node;
    }
    
    /* Go up until we are a left child */
    while (node->parent && node == node->parent->right) {
        node = node->parent;
    }
    return node->parent;
}

size_t
piece_table_iter_get_next_line(PieceTableIter *iter, char *buf, size_t buf_len)
{
    if (!iter || !iter->current_node) return 0;
    
    size_t copied = 0;
    
    while (iter->current_node) {
        const char *data = get_piece_data(iter->pt, &iter->current_node->piece);
        data += iter->current_node->piece.start;
        size_t len = iter->current_node->piece.length;
        
        const char *ptr = data + iter->offset_in_node;
        const char *end = data + len;
        
        if (ptr >= end) {
             /* Should not happen usually, unless offset_in_node == len (empty node or just consumed?) 
                Move to next node. */
             iter->current_node = node_next_in_order(iter->current_node);
             iter->offset_in_node = 0;
             continue;
        }

        /* Search for newline */
        int nl_len;
        const char *nl = find_next_newline(ptr, end, &nl_len);
        
        if (nl) {
            size_t seg_len = nl - ptr + nl_len;
            if (buf && copied < buf_len) {
                size_t copy = seg_len;
                if (copied + copy > buf_len) copy = buf_len - copied;
                memcpy(buf + copied, ptr, copy);
            }
            copied += seg_len;
            iter->offset_in_node += seg_len;
            iter->current_line_index++;
            return copied;
        } else {
             size_t seg_len = end - ptr;
             if (buf && copied < buf_len) {
                size_t copy = seg_len;
                if (copied + copy > buf_len) copy = buf_len - copied;
                memcpy(buf + copied, ptr, copy);
            }
            copied += seg_len;
            
            /* Move to next node */
            iter->current_node = node_next_in_order(iter->current_node);
            iter->offset_in_node = 0;
             /* Continue loop to append next chunk */
        }
    }
    
    /* EOF reached. If we copied something, it's the last line (no newline). */
    if (copied > 0) iter->current_line_index++;
    return copied;
}

size_t
piece_table_iter_get_next_line_string(PieceTableIter *iter, GString *buf)
{
    if (!iter || !iter->current_node || !buf) return 0;
    
    size_t copied = 0;
    
    while (iter->current_node) {
        const char *data = get_piece_data(iter->pt, &iter->current_node->piece);
        data += iter->current_node->piece.start;
        size_t len = iter->current_node->piece.length;
        
        const char *ptr = data + iter->offset_in_node;
        const char *end = data + len;
        
        if (ptr >= end) {
             iter->current_node = node_next_in_order(iter->current_node);
             iter->offset_in_node = 0;
             continue;
        }

        int nl_len;
        const char *nl = find_next_newline(ptr, end, &nl_len);
        
        if (nl) {
            size_t seg_len = nl - ptr + nl_len;
            g_string_append_len(buf, ptr, seg_len);
            copied += seg_len;
            iter->offset_in_node += seg_len;
            iter->current_line_index++;
            return copied;
        } else {
             size_t seg_len = end - ptr;
             g_string_append_len(buf, ptr, seg_len);
             copied += seg_len;
            
            iter->current_node = node_next_in_order(iter->current_node);
            iter->offset_in_node = 0;
        }
    }
    
    if (copied > 0) iter->current_line_index++;
    return copied;
}

void
piece_table_iter_init_at_line(PieceTable *pt, PieceTableIter *iter, size_t line_index)
{
    if (!pt || !iter) return;
    memset(iter, 0, sizeof(PieceTableIter));
    iter->pt = pt;
    
    PieceNode *curr = pt->root;
    if (!curr) return;
    
    /* Clamp */
    if (line_index >= curr->lf_subtree) {
         /* Set to end */
         iter->current_node = NULL;
         /* We don't really have a 'total_lines' field in iter, but current_line_index can hint */
         iter->current_line_index = curr->lf_subtree;
         return;
    }
    
    size_t accumulated_lines = 0;
    
    while (curr) {
        size_t left_lf = curr->left ? curr->left->lf_subtree : 0;
        
        if (line_index < accumulated_lines + left_lf) {
            curr = curr->left;
        } else {
            accumulated_lines += left_lf;
            size_t node_lf = curr->piece.cached_lf;
            
            if (line_index <= accumulated_lines + node_lf) {
                /* Found */
                iter->current_node = curr;
                iter->current_line_index = line_index;
                
                size_t lines_to_skip = line_index - accumulated_lines;
                
                const char *data = get_piece_data(pt, &curr->piece);
                data += curr->piece.start;
                const char *ptr = data;
                const char *end = data + curr->piece.length;
                
                for (size_t i = 0; i < lines_to_skip; i++) {
                    int nl_len;
                    const char *nl = find_next_newline(ptr, end, &nl_len);
                    if (nl) {
                        ptr = nl + nl_len;
                    } else {
                        break; 
                    }
                }
                iter->offset_in_node = ptr - data;
                return;
            }
            
            accumulated_lines += node_lf;
            curr = curr->right;
        }
    }
    iter->current_node = NULL;
}

/* Chunk Iterator API */
const char *
piece_table_iter_get_chunk(PieceTableIter *iter, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!iter || !iter->current_node) return NULL;
    
    /* Loop to skip empty nodes */
    while (iter->current_node) {
        size_t len = iter->current_node->piece.length;
        if (iter->offset_in_node < len) {
            /* Found data */
            const char *data = get_piece_data(iter->pt, &iter->current_node->piece);
            data += iter->current_node->piece.start + iter->offset_in_node;
            
            if (out_len) *out_len = len - iter->offset_in_node;
            return data;
        }
        
        /* Node exhausted, move to next */
        iter->current_node = node_next_in_order(iter->current_node);
        iter->offset_in_node = 0;
    }
    
    return NULL;
}

void
piece_table_iter_advance(PieceTableIter *iter, size_t len)
{
    if (!iter || !iter->current_node) return;
    
    iter->offset_in_node += len;
    
    /* If we exceeded or matched node length, move to next */
    while (iter->current_node && iter->offset_in_node >= iter->current_node->piece.length) {
        size_t excess = iter->offset_in_node - iter->current_node->piece.length;
        
        iter->current_node = node_next_in_order(iter->current_node);

        iter->offset_in_node = excess; /* Should be 0 usually if we consume exactly chunk, 
                                          but if we advanced arbitrary amount, we carry over */
    }
}
char *
piece_table_get_line_truncated(PieceTable *pt, size_t line_index, size_t *out_len, size_t max_len, size_t *out_full_len)
{
    size_t total_scanned_len = 0;
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    
    if (!node) {
        *out_len = 0;
        if (out_full_len) *out_full_len = 0;
        return g_strdup("");
    }
    
    size_t relative_lf = line_index - start_lf;
    const char *data = get_piece_data(pt, &node->piece);
    data += node->piece.start;
    size_t len = node->piece.length;
    
    size_t internal_offset = 0;
    int nl_len;
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = data;
        const char *end = data + len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - data;
    }
    
    GString *res = g_string_new("");
    const char *ptr = data + internal_offset;
    const char *node_end = data + len;
    
    PieceNode *curr = node;
    /* gboolean found_eol = FALSE; -- Unused */
    
    /* Loop through nodes until EOL found */
    while (curr) {
        const char *eol = find_next_newline(ptr, node_end, &nl_len);
        
        if (eol) {
            size_t chunk = eol - ptr + nl_len;
            total_scanned_len += chunk;
            
            if (res->len < max_len) {
                size_t append_len = chunk;
                if (res->len + append_len > max_len) append_len = max_len - res->len;
                g_string_append_len(res, ptr, append_len);
            }
            /* found_eol = TRUE; */
            break;
        } else {
            size_t chunk = node_end - ptr;
            total_scanned_len += chunk;
            
            if (res->len < max_len) {
                size_t append_len = chunk;
                if (res->len + append_len > max_len) append_len = max_len - res->len;
                g_string_append_len(res, ptr, append_len);
            } else if (!out_full_len) {
                /* Buffer full and no need to calculate full length */
                break;
            }
            
            /* Advance to next node */
            if (curr->right) {
                curr = curr->right;
                while (curr->left) curr = curr->left;
            } else {
                while (curr->parent && curr == curr->parent->right) {
                    curr = curr->parent;
                }
                curr = curr->parent;
            }
            
            if (!curr) break;
            
            data = get_piece_data(pt, &curr->piece);
            ptr = data + curr->piece.start;
            node_end = ptr + curr->piece.length;
        }
    }
    
    *out_len = res->len;
    if (out_full_len) *out_full_len = total_scanned_len;
    
    return g_string_free(res, FALSE);
}

/* -- Async Loading Implementation -- */

typedef struct {
    char *data;
    size_t size;
    gboolean is_mmapped;
    FileEncoding encoding;
    NewlineType newline_style;
    gboolean has_bom;
    char *mmap_base;
    size_t mmap_size;
    char *temp_path;  /* Temp file for UTF-16 conversion */
    PieceNode *root;
    PieceNode **temp_nodes; /* Used during build */
    size_t node_count;
} LoadResult;

static void
piece_table_load_data_free(PieceTableLoadData *data)
{
    if (!data) return;
    g_free(data->filename);
    if (data->cancellable) g_object_unref(data->cancellable);
    g_free(data);
}

static void
load_result_free(LoadResult *res)
{
    if (res->root) {
        free_tree(res->root); 
    } else if (res->temp_nodes) {
        if (res->node_count > 0 && res->temp_nodes) {
             for (size_t i = 0; i < res->node_count; i++) {
                 if (res->temp_nodes[i]) {
                     free_tree(res->temp_nodes[i]);
                 }
             }
        }
    }
    
    if (res->temp_nodes) free(res->temp_nodes);
    
    /* Clean up temp file if present */
    if (res->temp_path) {
        unlink(res->temp_path);
        g_free(res->temp_path);
    }
    
    if (res->is_mmapped && res->mmap_base && res->mmap_size > 0) {
         munmap(res->mmap_base, res->mmap_size);
    } else if (res->data && !res->is_mmapped) {
        g_free(res->data);
    }
    g_free(res);
}

/* Idle callback to dispatch progress on main thread */
typedef struct {
    PieceTableLoadProgressCallback cb;
    gpointer data;
    double progress;
    FileEncoding encoding;
    NewlineType newline;
} IdleProgressData;

static gboolean
dispatch_progress_idle(gpointer user_data)
{
    IdleProgressData *info = user_data;
    if (info->cb) {
        info->cb(info->progress, info->encoding, info->newline, info->data);
    }
    g_free(info);
    return G_SOURCE_REMOVE; 
}

static void
queue_load_progress(PieceTableLoadData *data, double progress, FileEncoding encoding, NewlineType newline)
{
    if (!data || !data->progress_cb) return;
    IdleProgressData *info = g_new0(IdleProgressData, 1);
    info->cb = data->progress_cb;
    info->data = data->progress_data;
    info->progress = progress;
    info->encoding = encoding;
    info->newline = newline;
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, dispatch_progress_idle, info, NULL);
}

/* Thread worker */
static void
load_file_worker(GTask *task, gpointer source_object G_GNUC_UNUSED, gpointer task_data, GCancellable *cancellable)
{
    PieceTableLoadData *data = task_data;
    const char *filename = data->filename;
    
    /* DEBUG: Timing start */
    GTimer *timer = g_timer_new();
    g_print("[LOAD DEBUG] Starting load: %s\n", filename);
    
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        g_task_return_new_error(task, G_FILE_ERROR, g_file_error_from_errno(errno), 
                                "Failed to open file: %s", strerror(errno));
        return;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        close(fd);
        g_task_return_new_error(task, G_FILE_ERROR, g_file_error_from_errno(errno), 
                                "Failed to stat file: %s", strerror(errno));
        return;
    }
    
    size_t size = sb.st_size;
    char *mmap_ptr = NULL;
    if (size > 0) {
        mmap_ptr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    }
    close(fd);
    
    if (size > 0 && (mmap_ptr == MAP_FAILED || mmap_ptr == NULL)) {
        g_task_return_new_error(task, G_FILE_ERROR, g_file_error_from_errno(errno), 
                                "Failed to mmap file: %s (ptr=%p)", strerror(errno), mmap_ptr);
        return;
    }

    LoadResult *res = g_new0(LoadResult, 1);
    res->mmap_base = mmap_ptr;
    res->mmap_size = size;
    res->is_mmapped = (size > 0);
    res->data = mmap_ptr;
    res->size = size;

    /* Detect Encoding */
    size_t bom_len = 0;
    gboolean did_conversion = FALSE;
    const double conversion_share = 0.2;
    if (res->data && res->size > 0) {
        detect_encoding(res->data, res->size, &res->encoding, &res->has_bom, &bom_len);
        
        /* Detect newline style immediately (raw check) */
        detect_newline_style_raw(res->data + bom_len, res->size - bom_len, res->encoding, &res->newline_style);

        /* 0. Send immediate progress to update Status Bar with Encoding info */
        queue_load_progress(data, 0.0, res->encoding, res->newline_style);
        
        /* DEBUG: After encoding detection */
        const char *enc_name = file_encoding_to_display_name(res->encoding);
        g_print("[LOAD DEBUG] %.3fs - Detected encoding: %s, Size: %.2f MB\n", 
                g_timer_elapsed(timer, NULL), enc_name, (double)res->size / (1024*1024));
        
        if (g_cancellable_is_cancelled(cancellable)) {
            load_result_free(res);
            g_task_return_error_if_cancelled(task);
            g_timer_destroy(timer);
            return;
        }

        if (res->encoding != ENCODING_UTF8) {
            did_conversion = TRUE;
            /* Disk-backed conversion to UTF-8 for zero-RAM storage. */
            const char *from_codeset = file_encoding_to_charset(res->encoding);
            
            /* Create temp file for converted UTF-8 data */
            const char *tmp_dir = g_get_tmp_dir();
            if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
            char *temp_template = g_strdup_printf("%s/vite-utf8-XXXXXX", tmp_dir);
            int temp_fd = mkstemp(temp_template);
            if (temp_fd >= 0) {
                unlink(temp_template); /* Crash safety */
            } else {
                /* Fall back to RAM-based conversion */
                g_free(temp_template);
                goto ram_fallback;
            }
            g_free(temp_template);

            /* For stateful/multibyte encodings, prefer one-shot fallback conversion
               to avoid split-sequence artifacts in chunked conversion. */
            if (!file_encoding_is_stream_safe(res->encoding)) {
                close(temp_fd);
                goto ram_fallback;
            }
            
            /* Stream-convert chunks from UTF-16 to UTF-8 */
            const char *src = res->data + bom_len;
            size_t src_remaining = res->size - bom_len;
            size_t conv_chunk_size = UTF16_CONVERT_CHUNK_SIZE; /* 64KB chunks */
            size_t total_written = 0;
            const size_t convert_total = res->size > bom_len ? (res->size - bom_len) : 0;
            gint64 last_progress = 0;
            
            gint64 work_budget_us = 0;
            while (src_remaining > 0) {
                gint64 chunk_start = g_get_monotonic_time();
                if (g_cancellable_is_cancelled(cancellable)) {
                    close(temp_fd);
                    load_result_free(res);
                    g_task_return_error_if_cancelled(task);
                    return;
                }
                
                size_t to_convert = (src_remaining < conv_chunk_size) ? src_remaining : conv_chunk_size;
                
                if (file_encoding_is_utf16(res->encoding)) {
                    /* Ensure strictly even bytes for UTF-16 */
                    if ((to_convert % 2) != 0) {
                        to_convert--;
                    }
                    
                    /* Check for split surrogate pair at end of chunk */
                    if (to_convert < src_remaining && to_convert >= 2) {
                        guint16 last_unit;
                        memcpy(&last_unit, src + to_convert - 2, 2);
                        if (res->encoding == ENCODING_UTF16BE) {
                            last_unit = GUINT16_FROM_BE(last_unit);
                        } else {
                            last_unit = GUINT16_FROM_LE(last_unit);
                        }
                        
                        /* High surrogate range: 0xD800 - 0xDBFF */
                        if (last_unit >= 0xD800 && last_unit <= 0xDBFF) {
                            /* Last unit is a high surrogate, and we know we have more data 
                                (because to_convert < src_remaining).
                                The low surrogate is in the next chunk.
                                We must exclude this high surrogate from this chunk. */
                            to_convert -= 2;
                        }
                    }
                } else if (file_encoding_is_utf32(res->encoding)) {
                    /* Ensure UTF-32 code-point alignment */
                    if ((to_convert % 4) != 0) {
                        to_convert -= (to_convert % 4);
                    }
                }
                if (to_convert == 0) {
                    close(temp_fd);
                    goto ram_fallback;
                }
                
                gsize bytes_read, bytes_written;
                GError *conv_error = NULL;
                char *utf8_chunk = g_convert(src, to_convert, "UTF-8", from_codeset,
                                             &bytes_read, &bytes_written, &conv_error);
                
                if (!utf8_chunk) {
                    g_clear_error(&conv_error);
                    close(temp_fd);
                    goto ram_fallback;
                }
                
                /* Write to temp file */
                ssize_t written = write(temp_fd, utf8_chunk, bytes_written);
                g_free(utf8_chunk);
                
                if (written != (ssize_t)bytes_written) {
                    close(temp_fd);
                    goto ram_fallback;
                }
                
                total_written += bytes_written;
                src += bytes_read;
                src_remaining -= bytes_read;
                work_budget_us += (g_get_monotonic_time() - chunk_start);
                utf16_convert_throttle(res->size, &work_budget_us);

                if (data->progress_cb && convert_total > 0) {
                    gint64 now = g_get_monotonic_time();
                    if (now - last_progress > 100 * 1000) {
                        double p = (double)(convert_total - src_remaining) / (double)convert_total;
                        queue_load_progress(data, conversion_share * p, res->encoding, res->newline_style);
                        last_progress = now;
                    }
                }
            }
            if (data->progress_cb) {
                queue_load_progress(data, conversion_share, res->encoding, res->newline_style);
            }
            
            /* Munmap original UTF-16 file */
            if (res->mmap_base && res->mmap_size > 0) {
                munmap(res->mmap_base, res->mmap_size);
            }
            
            /* mmap the temp file */
            if (total_written > 0) {
                char *utf8_mmap = mmap(NULL, total_written, PROT_READ, MAP_PRIVATE, temp_fd, 0);
                close(temp_fd);
                
                if (utf8_mmap == MAP_FAILED) {
                    goto ram_fallback;
                } else {
                    res->mmap_base = utf8_mmap;
                    res->mmap_size = total_written;
                    res->data = utf8_mmap;
                    res->size = total_written;
                    res->is_mmapped = TRUE;
                }
            } else {
                close(temp_fd);
                res->mmap_base = NULL;
                res->mmap_size = 0;
                res->data = NULL;
                res->size = 0;
                res->is_mmapped = FALSE;
            }
            goto conversion_done;
            
ram_fallback:
            /* RAM-based fallback */
            {
                gsize bytes_read, bytes_written;
                GError *error = NULL;
                char *utf8_data = g_convert(res->data + bom_len, res->size - bom_len, "UTF-8", from_codeset, &bytes_read, &bytes_written, &error);
                
                if (utf8_data) {
                    if (res->is_mmapped) munmap(res->mmap_base, res->mmap_size);
                    res->mmap_base = NULL;
                    res->mmap_size = 0;
                    res->is_mmapped = FALSE;
                    res->data = utf8_data;
                    res->size = bytes_written;
                    queue_load_progress(data, conversion_share, res->encoding, res->newline_style);
                } else {
                    load_result_free(res);
                    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Encoding conversion failed: %s", error->message);
                    g_error_free(error);
                    return;
                }
            }
conversion_done:
            (void)0; /* Empty statement for label */
        } else if (bom_len > 0) {
            res->data += bom_len;
            res->size -= bom_len;
        }
        
        detect_newline_style(res->data, res->size, &res->newline_style);
    }

    /* Build Tree */
    if (res->size > 0) {
        size_t chunk_size = 16 * 1024;
        size_t count = (res->size + chunk_size - 1) / chunk_size;
        
        res->temp_nodes = calloc(count, sizeof(PieceNode*));
        res->node_count = count;
        
        /* Optimization: Hint OS that we are scanning sequentially.
           This allows the kernel to discard pages aggressively after we read them, keeps RSS low. */
#ifdef MADV_SEQUENTIAL
        madvise(res->mmap_base, res->mmap_size, MADV_SEQUENTIAL);
#endif
        


        /* 1. Create nodes */
        for (size_t i = 0; i < count; i++) {
            if ((i % 128) == 0) {
                 if (g_cancellable_is_cancelled(cancellable)) {

                     load_result_free(res);
                     g_task_return_error_if_cancelled(task);
                     return;
                 }
                 
                 
                 if (data->progress_cb) {
                     double p = (double)i / count;
                     double base = did_conversion ? conversion_share : 0.0;
                     double scale = did_conversion ? (1.0 - conversion_share) : 1.0;
                     queue_load_progress(data, base + (scale * p), res->encoding, res->newline_style);
                 }
            }

            size_t start = i * chunk_size;
            size_t len = chunk_size;
            if (start + len > res->size) len = res->size - start;
            


            size_t lf = count_newlines(res->data + start, len);
            Piece p = { SOURCE_ORIGINAL, start, len, lf };
            res->temp_nodes[i] = node_new(p);
        }

#ifdef MADV_NORMAL
        /* Restore normal access pattern for interactive use */
        madvise(res->mmap_base, res->mmap_size, MADV_NORMAL);
#endif
         
        
        /* 2. Build Tree */
        res->root = build_balanced_tree_recursive(res->temp_nodes, 0, (int)count - 1, NULL);
        
        free(res->temp_nodes);
        res->temp_nodes = NULL;
        res->node_count = 0;
    }
    

    g_timer_destroy(timer);
    
    g_task_return_pointer(task, res, (GDestroyNotify)load_result_free);
}


PieceTable *
piece_table_new_empty(void)
{
    PieceTable *pt = g_new0(PieceTable, 1);
    pt->add_buffer = disk_buffer_new();
    pt->root = NULL;
    return pt;
}

void
piece_table_load_async(PieceTable *pt, const char *filename, GCancellable *cancellable, 
                            PieceTableLoadProgressCallback progress_cb, gpointer progress_data,
                            GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    PieceTableLoadData *data = g_new0(PieceTableLoadData, 1);
    data->pt = pt; 
    data->filename = g_strdup(filename);
    data->progress_cb = progress_cb;
    data->progress_data = progress_data;
    data->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    
    g_task_set_task_data(task, data, (GDestroyNotify)piece_table_load_data_free);
    g_task_run_in_thread(task, load_file_worker);
    g_object_unref(task);
}

gboolean
piece_table_load_finish(PieceTable *pt, GAsyncResult *res, GError **error)
{
    LoadResult *lr = g_task_propagate_pointer(G_TASK(res), error);
    if (!lr) return FALSE;
    
    /* Clean up old state */
    if (pt->is_mmapped && pt->mmap_base && pt->mmap_size > 0) {
        munmap(pt->mmap_base, pt->mmap_size);
    } else if (pt->orig_data) {
        g_free(pt->orig_data);
    }
    if (pt->temp_path) {
        unlink(pt->temp_path);
        g_free(pt->temp_path);
        pt->temp_path = NULL;
    }
    free_tree(pt->root);
    piece_table_clear_external_sources(pt);
    
    /* Apply new state */
    pt->mmap_base = lr->mmap_base;
    pt->mmap_size = lr->mmap_size;
    pt->is_mmapped = lr->is_mmapped;
    pt->orig_data = lr->data;
    pt->orig_size = lr->size;
    pt->root = lr->root;
    pt->encoding = lr->encoding;
    pt->newline_style = lr->newline_style;
    pt->has_bom = lr->has_bom;
    pt->temp_path = lr->temp_path;  /* Transfer ownership of temp file */
    
    /* Clear from LR so they aren't freed */
    lr->mmap_base = NULL;
    lr->mmap_size = 0;
    lr->data = NULL;
    lr->size = 0;
    lr->root = NULL;
    lr->temp_path = NULL;  /* Don't let load_result_free delete it */
    lr->is_mmapped = FALSE;
    lr->node_count = 0;
    lr->temp_nodes = NULL;
    
    /* Reset add buffer */
    disk_buffer_free(pt->add_buffer);
    pt->add_buffer = disk_buffer_new();
    pt->change_count = 0;
    
    load_result_free(lr);
    return TRUE;
}




static void
insert_piece_at_offset(PieceTable *pt, size_t offset, Piece new_piece)
{
    pt->change_count++;
    
    PieceNode *new_node = node_new(new_piece);
    
    if (!pt->root) {
        pt->root = new_node;
        update_node(pt, pt->root);
        return;
    }
    
    size_t node_start_off;
    PieceNode *at_node = find_node_at_offset(pt, offset, &node_start_off);
    
    if (!at_node) {
        PieceNode *curr = pt->root;
        while (curr->right) curr = curr->right;
        curr->right = new_node;
        new_node->parent = curr;
        splay(pt, new_node);
        return;
    }
    
    size_t split_point = offset - node_start_off;
    
    if (split_point == 0) {
        new_node->left = at_node->left;
        if (new_node->left) new_node->left->parent = new_node;
        new_node->right = at_node;
        at_node->parent = new_node;
        at_node->left = NULL; 
        pt->root = new_node;
        update_node(pt, at_node);
        update_node(pt, new_node);
    } else if (split_point == at_node->piece.length) {
        new_node->right = at_node->right;
        if (new_node->right) new_node->right->parent = new_node;
        new_node->left = at_node;
        at_node->parent = new_node;
        at_node->right = NULL;
        pt->root = new_node;
        update_node(pt, at_node);
        update_node(pt, new_node);
    } else {
        Piece right_p = at_node->piece;
        right_p.start += split_point;
        right_p.length -= split_point;
        right_p.cached_lf = count_newlines(get_piece_data(pt, &at_node->piece) + right_p.start, right_p.length);
        
        at_node->piece.length = split_point;
        at_node->piece.cached_lf = count_newlines(get_piece_data(pt, &at_node->piece) + at_node->piece.start, split_point);
        
        PieceNode *right_node = node_new(right_p);
        
        right_node->right = at_node->right;
        if (right_node->right) right_node->right->parent = right_node;
        
        new_node->left = at_node;
        at_node->parent = new_node;
        at_node->right = NULL;
        
        new_node->right = right_node;
        right_node->parent = new_node;
        
        update_node(pt, at_node);
        update_node(pt, right_node);
        update_node(pt, new_node);
        pt->root = new_node;
    }
}

/* Insert range from FD (mmap) */
void piece_table_insert_from_fd_range(PieceTable *pt, size_t offset, int fd, size_t file_offset, size_t len)
{
    if (!pt || len == 0 || fd < 0) return;
    
    struct stat sb;
    if (fstat(fd, &sb) < 0) return;
    
    if (sb.st_size < (off_t)(file_offset + len)) {
        if ((off_t)file_offset < sb.st_size)
             len = sb.st_size - file_offset;
        else
             return;
    }
    
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    
    off_t aligned_offset = (file_offset / page_size) * page_size;
    size_t alignment_diff = file_offset - aligned_offset;
    size_t map_len = len + alignment_diff;
    
    void *map = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, aligned_offset);
    if (map == MAP_FAILED) {
        g_warning("piece_table_insert_from_fd_range: mmap failed");
        return;
    }
    
    int new_fd = dup(fd);
    
    PieceTableSource *src = g_new0(PieceTableSource, 1);
    src->fd = new_fd;
    src->mmap_base = map;
    src->size = map_len;
    
    if (!pt->external_sources) {
        pt->external_sources = g_ptr_array_new_with_free_func(g_free);
        /* dummy check to satisfy my paranoia */
        if (pt->external_sources) g_ptr_array_unref(pt->external_sources); 
        pt->external_sources = g_ptr_array_new();
    }
    g_ptr_array_add(pt->external_sources, src);
    guint source_id = SOURCE_EXTERNAL_START + (pt->external_sources->len - 1);
    
    /* Chunk Insertion to avoid O(N) line traversal on huge pieces */
    size_t chunk_size = 64 * 1024; /* 64KB pieces */
    size_t current_processed = 0;
    size_t current_doc_off = offset;
    
    while (current_processed < len) {
        size_t chunk = chunk_size;
        if (current_processed + chunk > len) chunk = len - current_processed;
        
        /* Calculate pointer to this chunk's data */
        /* map points to aligned_offset. 
           Data starts at aligned_offset + alignment_diff.
           Current chunk starts at aligned_offset + alignment_diff + current_processed.
           
           Offset relative to map (mmap_base): alignment_diff + current_processed
        */
        size_t offset_in_map = alignment_diff + current_processed;
        char *chunk_ptr = (char *)map + offset_in_map;

        size_t chunk_lf = count_newlines(chunk_ptr, chunk);
        
        /* Piece offset is relative to the source's mmap_base */
        Piece p = { (PieceSource)source_id, offset_in_map, chunk, chunk_lf };
        
        insert_piece_at_offset(pt, current_doc_off, p);
        
        current_processed += chunk;
        current_doc_off += chunk;
    }
}

void
piece_table_insert_from_fd(PieceTable *pt, size_t offset, int fd, size_t len, size_t lf_count G_GNUC_UNUSED)
{
    if (len == 0 || fd < 0) return;
    
    struct stat sb;
    fstat(fd, &sb);
    if (sb.st_size < (off_t)len) len = sb.st_size;
    
    char *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        g_warning("piece_table_insert_from_fd: mmap failed");
        return;
    }
    
    int new_fd = dup(fd);
    
    PieceTableSource *src = g_new0(PieceTableSource, 1);
    src->fd = new_fd;
    src->mmap_base = map;
    src->size = len;
    
    if (!pt->external_sources) {
        pt->external_sources = g_ptr_array_new_with_free_func(g_free);
        /* dummy check to satisfy my paranoia */
        if (pt->external_sources) g_ptr_array_unref(pt->external_sources); 
        pt->external_sources = g_ptr_array_new();
    }
    g_ptr_array_add(pt->external_sources, src);
    guint source_id = SOURCE_EXTERNAL_START + (pt->external_sources->len - 1);
    
    /* Chunk Insertion to avoid O(N) line traversal on huge pieces */
    size_t chunk_size = 64 * 1024; /* 64KB pieces */
    size_t current_file_off = 0;
    size_t current_doc_off = offset;
    
    while (current_file_off < len) {
        size_t chunk = chunk_size;
        if (current_file_off + chunk > len) chunk = len - current_file_off;
        
        size_t chunk_lf = count_newlines(map + current_file_off, chunk);
        Piece p = { (PieceSource)source_id, current_file_off, chunk, chunk_lf };
        
        insert_piece_at_offset(pt, current_doc_off, p);
        
        current_file_off += chunk;
        current_doc_off += chunk;
    }
}

void
piece_table_set_newline_type(PieceTable *pt, NewlineType type)
{
    if (pt) pt->newline_style = type;
}

NewlineType
piece_table_get_newline_type(PieceTable *pt)
{
    return pt ? pt->newline_style : NEWLINE_LF;
}

void
piece_table_set_encoding(PieceTable *pt, FileEncoding enc)
{
    if (pt) pt->encoding = enc;
}

FileEncoding
piece_table_get_encoding(PieceTable *pt)
{
    return pt ? pt->encoding : ENCODING_UTF8;
}

/* ============================================================================
 * Encoding Loss Pre-Check - Zero-write probe of encoding compatibility
 * ============================================================================ */

/**
 * piece_table_check_encoding_lossy:
 * @pt: the piece table
 *
 * Checks whether saving the document to its current target encoding would
 * lose any characters (i.e., require '?' substitutions).  This is a read-only
 * probe — nothing is written to disk.
 *
 * Returns: TRUE if saving would be lossy (some chars can't be represented),
 *          FALSE if the content can be losslessly converted.
 */
gboolean
piece_table_check_encoding_lossy(PieceTable *pt)
{
    if (!pt) return FALSE;
    if (pt->encoding == ENCODING_UTF8) return FALSE; /* UTF-8 is always lossless */

    const char *target_charset = file_encoding_to_charset(pt->encoding);
    if (!target_charset) return FALSE;

    PieceTableIter iter;
    piece_table_iter_init(pt, &iter);

    size_t chunk_len;
    const char *chunk;

    char pending_bytes[6];
    size_t pending_len = 0;

    while ((chunk = piece_table_iter_get_chunk(&iter, &chunk_len)) != NULL) {
        if (chunk_len == 0) {
            piece_table_iter_advance(&iter, 1);
            continue;
        }

        /* Combine pending bytes with current chunk */
        char *combined = NULL;
        const char *working_data = chunk;
        size_t working_len = chunk_len;
        
        if (pending_len > 0) {
            combined = g_malloc(pending_len + chunk_len);
            memcpy(combined, pending_bytes, pending_len);
            memcpy(combined + pending_len, chunk, chunk_len);
            working_data = combined;
            working_len = pending_len + chunk_len;
            pending_len = 0;
        }

        /* Find the last complete UTF-8 character */
        size_t safe_len = working_len;
        {
            /* Check if the last few bytes form a complete UTF-8 sequence */
            const char *p = working_data + working_len;
            size_t trailing = 0;
            
            /* Scan backwards to find start of last UTF-8 character */
            while (trailing < 4 && trailing < working_len) {
                p--;
                trailing++;
                unsigned char c = (unsigned char)*p;
                if ((c & 0x80) == 0) {
                    /* ASCII - complete */
                    trailing = 0;
                    break;
                } else if ((c & 0xC0) == 0xC0) {
                    /* Start of multi-byte sequence - check if complete */
                    size_t expected = 0;
                    if ((c & 0xE0) == 0xC0) expected = 2;
                    else if ((c & 0xF0) == 0xE0) expected = 3;
                    else if ((c & 0xF8) == 0xF0) expected = 4;
                    
                    if (trailing < expected) {
                        /* Incomplete sequence - save for next chunk */
                        safe_len = working_len - trailing;
                        memcpy(pending_bytes, working_data + safe_len, trailing);
                        pending_len = trailing;
                    } else {
                        trailing = 0;  /* Complete */
                    }
                    break;
                }
                /* else continuation byte, keep scanning */
            }
        }

        if (safe_len > 0) {
            /* Ensure the chunk is valid UTF-8 first */
            if (!g_utf8_validate(working_data, (gssize)safe_len, NULL)) {
                g_free(combined);
                return TRUE; /* Invalid UTF-8 bytes = will need substitution */
            }

            /* Try strict conversion with no fallback */
            GError *conv_error = NULL;
            gsize bytes_read, bytes_written;
            char *converted = g_convert(working_data, (gssize)safe_len,
                                        target_charset, "UTF-8",
                                        &bytes_read, &bytes_written, &conv_error);
            if (!converted) {
                g_clear_error(&conv_error);
                g_free(combined);
                return TRUE; /* This chunk has unencodable characters */
            }
            g_free(converted);
        }

        g_free(combined);
        piece_table_iter_advance(&iter, chunk_len);
    }

    /* Final check: if we have pending bytes at the end, they must be invalid */
    if (pending_len > 0) {
        return TRUE;
    }

    return FALSE; /* All content can be losslessly converted */
}

/* ============================================================================
 * Streaming Save Implementation - Zero-RAM file saving
 * ============================================================================ */

gboolean
piece_table_save_to_fd(PieceTable *pt, int fd, GError **error)
{
    if (!pt) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "NULL piece table");
        return FALSE;
    }
    
    PieceTableIter iter;
    piece_table_iter_init(pt, &iter);
    
    gboolean need_crlf = (pt->newline_style == NEWLINE_CRLF);
    gboolean need_conversion = (pt->encoding != ENCODING_UTF8);
    const char *target_charset = file_encoding_to_charset(pt->encoding);
    
    /* Write BOM - for UTF-16 files, always write BOM; for UTF-8 only if original had it */
    if (file_encoding_is_utf16(pt->encoding) || file_encoding_is_utf32(pt->encoding) || pt->has_bom) {
        if (pt->encoding == ENCODING_UTF8 && pt->has_bom) {
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            if (write(fd, bom, 3) != 3) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write BOM: %s", strerror(errno));
                return FALSE;
            }
        } else if (pt->encoding == ENCODING_UTF16LE) {
            const unsigned char bom[] = { 0xFF, 0xFE };
            if (write(fd, bom, 2) != 2) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write BOM: %s", strerror(errno));
                return FALSE;
            }
        } else if (pt->encoding == ENCODING_UTF16BE) {
            const unsigned char bom[] = { 0xFE, 0xFF };
            if (write(fd, bom, 2) != 2) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write BOM: %s", strerror(errno));
                return FALSE;
            }
        } else if (pt->encoding == ENCODING_UTF32LE) {
            const unsigned char bom[] = { 0xFF, 0xFE, 0x00, 0x00 };
            if (write(fd, bom, 4) != 4) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write BOM: %s", strerror(errno));
                return FALSE;
            }
        } else if (pt->encoding == ENCODING_UTF32BE) {
            const unsigned char bom[] = { 0x00, 0x00, 0xFE, 0xFF };
            if (write(fd, bom, 4) != 4) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write BOM: %s", strerror(errno));
                return FALSE;
            }
        }
    }
    
    /* Helper to write data with retry on EINTR */
    #define WRITE_ALL(fd, buf, len) do { \
        const char *_p = (buf); \
        size_t _rem = (len); \
        while (_rem > 0) { \
            ssize_t _w = write((fd), _p, _rem); \
            if (_w < 0) { \
                if (errno == EINTR) continue; \
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Write failed: %s", strerror(errno)); \
                return FALSE; \
            } \
            _p += _w; _rem -= _w; \
        } \
    } while(0)
    
    /* For UTF-16 conversion, we need to handle incomplete UTF-8 sequences at chunk boundaries.
     * We use a small buffer to carry over incomplete bytes to the next chunk. */
    char pending_bytes[6];  /* Max UTF-8 char is 4 bytes, but be safe */
    size_t pending_len = 0;
    
    /* Stream chunks to file */
    size_t chunk_len;
    const char *chunk;
    
    while ((chunk = piece_table_iter_get_chunk(&iter, &chunk_len)) != NULL) {
        if (chunk_len == 0) {
            piece_table_iter_advance(&iter, 1);
            continue;
        }
        
        /* Combine pending bytes with current chunk */
        char *combined = NULL;
        const char *working_data = chunk;
        size_t working_len = chunk_len;
        
        if (pending_len > 0) {
            combined = g_malloc(pending_len + chunk_len);
            memcpy(combined, pending_bytes, pending_len);
            memcpy(combined + pending_len, chunk, chunk_len);
            working_data = combined;
            working_len = pending_len + chunk_len;
            pending_len = 0;
        }
        
        /* For UTF-16 conversion, find the last complete UTF-8 character */
        size_t safe_len = working_len;
        if (need_conversion) {
            /* Check if the last few bytes form a complete UTF-8 sequence */
            const char *p = working_data + working_len;
            size_t trailing = 0;
            
            /* Scan backwards to find start of last UTF-8 character */
            while (trailing < 4 && trailing < working_len) {
                p--;
                trailing++;
                unsigned char c = (unsigned char)*p;
                if ((c & 0x80) == 0) {
                    /* ASCII - complete */
                    trailing = 0;
                    break;
                } else if ((c & 0xC0) == 0xC0) {
                    /* Start of multi-byte sequence - check if complete */
                    size_t expected = 0;
                    if ((c & 0xE0) == 0xC0) expected = 2;
                    else if ((c & 0xF0) == 0xE0) expected = 3;
                    else if ((c & 0xF8) == 0xF0) expected = 4;
                    
                    if (trailing < expected) {
                        /* Incomplete sequence - save for next chunk */
                        safe_len = working_len - trailing;
                        memcpy(pending_bytes, working_data + safe_len, trailing);
                        pending_len = trailing;
                    } else {
                        trailing = 0;  /* Complete */
                    }
                    break;
                }
                /* else continuation byte, keep scanning */
            }
        }
        
        /* Step 1: Handle CRLF conversion if needed (on UTF-8 data before encoding) */
        char *crlf_data = NULL;
        const char *data_to_encode = working_data;
        size_t data_len = safe_len;
        
        if (need_crlf && safe_len > 0) {
            /* Convert LF to CRLF */
            GString *converted = g_string_sized_new(safe_len + 32);
            const char *ptr = working_data;
            const char *end = working_data + safe_len;
            
            while (ptr < end) {
                if (*ptr == '\n' && (ptr == working_data || *(ptr - 1) != '\r')) {
                    g_string_append(converted, "\r\n");
                } else {
                    g_string_append_c(converted, *ptr);
                }
                ptr++;
            }
            
            data_len = converted->len;
            crlf_data = g_string_free(converted, FALSE);
            data_to_encode = crlf_data;
        }
        
        /* Step 2: Handle encoding conversion */
        if (need_conversion && data_len > 0) {
            /* Sanitize: ensure input is valid UTF-8 before conversion;
               invalid byte sequences are replaced with the UTF-8 replacement char. */
            gchar *sanitized = NULL;
            const char *src_utf8 = data_to_encode;
            gsize src_len = data_len;
            if (!g_utf8_validate(data_to_encode, (gssize)data_len, NULL)) {
                GString *safe = g_string_sized_new(data_len);
                const char *p = data_to_encode;
                const char *end_p = data_to_encode + data_len;
                while (p < end_p) {
                    gunichar ch = g_utf8_get_char_validated(p, end_p - p);
                    if (ch == (gunichar)-1 || ch == (gunichar)-2) {
                        g_string_append(safe, "\xEF\xBF\xBD"); /* U+FFFD */
                        p++;
                    } else {
                        gchar buf[6];
                        gint n = g_unichar_to_utf8(ch, buf);
                        g_string_append_len(safe, buf, n);
                        p = g_utf8_next_char(p);
                    }
                }
                src_len = safe->len;
                sanitized = g_string_free(safe, FALSE);
                src_utf8 = sanitized;
            }

            gsize bytes_written;
            GError *conv_error = NULL;
            char *converted_data = g_convert_with_fallback(src_utf8, (gssize)src_len,
                                                            target_charset, "UTF-8",
                                                            "?",
                                                            NULL, &bytes_written, &conv_error);
            g_free(sanitized);
            g_free(crlf_data);
            g_free(combined);
            
            if (!converted_data) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Encoding conversion failed: %s",
                            conv_error ? conv_error->message : "Unknown error");
                g_clear_error(&conv_error);
                return FALSE;
            }
            
            WRITE_ALL(fd, converted_data, bytes_written);
            g_free(converted_data);
        } else if (data_len > 0) {
            /* UTF-8: Direct write */
            WRITE_ALL(fd, data_to_encode, data_len);
            g_free(crlf_data);
            g_free(combined);
        } else {
            g_free(crlf_data);
            g_free(combined);
        }
        
        piece_table_iter_advance(&iter, chunk_len);
    }
    
    /* Handle any remaining pending bytes (should not happen with valid UTF-8) */
    if (pending_len > 0) {
        /* Invalid UTF-8 at end - write as-is only when no conversion is requested. */
        if (!need_conversion) {
            WRITE_ALL(fd, pending_bytes, pending_len);
        }
    }
    
    #undef WRITE_ALL
    return TRUE;
}

/* Async Save Task */
struct _PieceTableSaveTask {
    PieceTable *pt;
    int fd;
    PieceTableIter iter;
    size_t total_bytes;
    size_t bytes_written;
    gboolean need_crlf;
    gboolean bom_written;
    gboolean cancelled;
    GError *error;
    
    /* Buffer for incomplete UTF-8 sequences at chunk boundaries */
    char pending_bytes[6];
    size_t pending_len;
    
    /* State for newline normalization */
    gboolean saved_cr;

    /* Set when encoding conversion replaced chars with '?' (lossy save) */
    gboolean had_lossy_conversion;
};

PieceTableSaveTask *
piece_table_save_async_start(PieceTable *pt, int fd)
{
    if (!pt) return NULL;
    
    PieceTableSaveTask *task = g_new0(PieceTableSaveTask, 1);
    task->pt = pt;
    task->fd = fd;
    task->total_bytes = piece_table_get_length(pt);
    task->bytes_written = 0;
    task->need_crlf = (pt->newline_style == NEWLINE_CRLF);
    
    /* Calculate if BOM write is needed */
    gboolean needs_bom = (file_encoding_is_utf16(pt->encoding) || file_encoding_is_utf32(pt->encoding) || pt->has_bom);
    
    task->bom_written = !needs_bom; /* If no BOM needed, mark as written/skipped */
    task->cancelled = FALSE;
    task->error = NULL;
    
    piece_table_iter_init(pt, &task->iter);
    
    return task;
}

gboolean
piece_table_save_async_step(PieceTableSaveTask *task, gint64 budget_us, double *progress_out)
{
    if (!task || task->cancelled) {
        if (progress_out) *progress_out = 1.0;
        return TRUE; /* Done */
    }
    
    gint64 start_us = g_get_monotonic_time();
    
    gboolean need_conversion = (task->pt->encoding != ENCODING_UTF8);
    const char *target_charset = file_encoding_to_charset(task->pt->encoding);

    /* Write BOM if needed */
    if (!task->bom_written) {
        if (task->pt->encoding == ENCODING_UTF8 && task->pt->has_bom) {
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            write(task->fd, bom, 3);
        } else if (task->pt->encoding == ENCODING_UTF16LE) {
            const unsigned char bom[] = { 0xFF, 0xFE };
            write(task->fd, bom, 2);
        } else if (task->pt->encoding == ENCODING_UTF16BE) {
            const unsigned char bom[] = { 0xFE, 0xFF };
            write(task->fd, bom, 2);
        } else if (task->pt->encoding == ENCODING_UTF32LE) {
            const unsigned char bom[] = { 0xFF, 0xFE, 0x00, 0x00 };
            write(task->fd, bom, 4);
        } else if (task->pt->encoding == ENCODING_UTF32BE) {
            const unsigned char bom[] = { 0x00, 0x00, 0xFE, 0xFF };
            write(task->fd, bom, 4);
        }
        task->bom_written = TRUE;
    }
    
    /* Process chunks within time budget */
    size_t chunk_len;
    const char *chunk;
    
    while ((chunk = piece_table_iter_get_chunk(&task->iter, &chunk_len)) != NULL) {
        if (task->cancelled) break;
        
        /* Check time budget */
        gint64 elapsed = g_get_monotonic_time() - start_us;
        if (elapsed >= budget_us) break;
        
        if (chunk_len == 0) {
            piece_table_iter_advance(&task->iter, 1);
            continue;
        }
        
        /* Cap write size to 64KB to avoid blocking and allow throttling */
        size_t bytes_to_process = chunk_len;
        if (bytes_to_process > 65536) {
            bytes_to_process = 65536;
        }

        /* Combine pending bytes with current chunk */
        char *combined = NULL;
        const char *working_data = chunk;
        size_t working_len = bytes_to_process;
        
        if (task->pending_len > 0) {
            combined = g_malloc(task->pending_len + bytes_to_process);
            memcpy(combined, task->pending_bytes, task->pending_len);
            memcpy(combined + task->pending_len, chunk, bytes_to_process);
            working_data = combined;
            working_len = task->pending_len + bytes_to_process;
            task->pending_len = 0;
        }
        
        /* For conversion, find the last complete UTF-8 character */
        size_t safe_len = working_len;
        if (need_conversion) {
            /* Check if the last few bytes form a complete UTF-8 sequence */
            const char *p = working_data + working_len;
            size_t trailing = 0;
            
            /* Scan backwards to find start of last UTF-8 character */
            while (trailing < 4 && trailing < working_len) {
                p--;
                trailing++;
                unsigned char c = (unsigned char)*p;
                if ((c & 0x80) == 0) {
                    /* ASCII - complete */
                    trailing = 0;
                    break;
                } else if ((c & 0xC0) == 0xC0) {
                    /* Start of multi-byte sequence - check if complete */
                    size_t expected = 0;
                    if ((c & 0xE0) == 0xC0) expected = 2;
                    else if ((c & 0xF0) == 0xE0) expected = 3;
                    else if ((c & 0xF8) == 0xF0) expected = 4;
                    
                    if (trailing < expected) {
                        /* Incomplete sequence - save for next chunk */
                        safe_len = working_len - trailing;
                        memcpy(task->pending_bytes, working_data + safe_len, trailing);
                        task->pending_len = trailing;
                    } else {
                        trailing = 0;  /* Complete */
                    }
                    break;
                }
                /* else continuation byte, keep scanning */
            }
        }
        
        /* 1. Newline Normalization */
        /* Targets: LF (\n), CRLF (\r\n), CR (\r) */
        char *normalized_data = NULL;
        const char *data_to_encode = working_data;
        size_t data_len = safe_len;
        
        /* We perform normalization if any newline conversion is needed.
           Since we support arbitrary conversion, we essentially always run this
           unless target is "As Is" (which isn't an option, we have a style).
           We assume the user wants the file to correspond to pt->newline_style.
        */
        
        if (safe_len > 0 || task->saved_cr) {
            GString *converted = g_string_sized_new(safe_len + 128);
            const char *ptr = working_data;
            const char *end = working_data + safe_len;
            
            const char *nl_str = "\n";
            if (task->pt->newline_style == NEWLINE_CRLF) nl_str = "\r\n";
            else if (task->pt->newline_style == NEWLINE_CR) nl_str = "\r";
            
            while (ptr < end) {
                char c = *ptr;
                
                if (task->saved_cr) {
                    if (c == '\n') {
                        /* Case: \r\n -> Newline */
                        g_string_append(converted, nl_str);
                        task->saved_cr = FALSE;
                        ptr++;
                        continue;
                    } else {
                        /* Case: \r followed by non-\n -> Isolated \r -> Newline */
                        g_string_append(converted, nl_str);
                        task->saved_cr = FALSE;
                        /* Fall through to process 'c' */
                    }
                }
                
                if (c == '\r') {
                    task->saved_cr = TRUE;
                } else if (c == '\n') {
                    /* Case: Isolated \n -> Newline */
                    g_string_append(converted, nl_str);
                } else {
                    g_string_append_c(converted, c);
                }
                
                ptr++;
            }
            
            normalized_data = g_string_free(converted, FALSE);
            data_to_encode = normalized_data;
            data_len = normalized_data ? strlen(normalized_data) : 0; /* ASCII-safe for UTF-8 */
        }
        
        /* 2. Encoding Conversion */
        if (need_conversion && data_len > 0) {
            /* Sanitize: replace invalid UTF-8 bytes with U+FFFD before converting.
               Any sanitization is itself lossy, so flag it. */
            gchar *sanitized = NULL;
            const char *src_utf8 = data_to_encode;
            gsize src_len = data_len;
            if (!g_utf8_validate(data_to_encode, (gssize)data_len, NULL)) {
                task->had_lossy_conversion = TRUE; /* Bad bytes in input = lossy */
                GString *safe = g_string_sized_new(data_len);
                const char *p = data_to_encode;
                const char *end_p = data_to_encode + data_len;
                while (p < end_p) {
                    gunichar ch = g_utf8_get_char_validated(p, end_p - p);
                    if (ch == (gunichar)-1 || ch == (gunichar)-2) {
                        g_string_append(safe, "\xEF\xBF\xBD"); /* U+FFFD */
                        p++;
                    } else {
                        gchar buf[6];
                        gint n = g_unichar_to_utf8(ch, buf);
                        g_string_append_len(safe, buf, n);
                        p = g_utf8_next_char(p);
                    }
                }
                src_len = safe->len;
                sanitized = g_string_free(safe, FALSE);
                src_utf8 = sanitized;
            }

            /* First try strict conversion (no substitution). */
            gsize bytes_conv_written;
            GError *conv_error = NULL;
            char *utf16_data = g_convert(src_utf8, (gssize)src_len,
                                          target_charset, "UTF-8",
                                          NULL, &bytes_conv_written, &conv_error);
            if (!utf16_data) {
                /* Strict conversion failed - some chars can't be represented.
                   Fall back to lossy conversion with '?' replacement. */
                g_clear_error(&conv_error);
                task->had_lossy_conversion = TRUE;
                utf16_data = g_convert_with_fallback(src_utf8, (gssize)src_len,
                                                      target_charset, "UTF-8",
                                                      "?",
                                                      NULL, &bytes_conv_written, &conv_error);
            }
            g_free(sanitized);
            
            if (!utf16_data) {
                if (task->error) g_error_free(task->error);
                task->error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Encoding conversion failed: %s", 
                            conv_error ? conv_error->message : "Unknown error");
                g_clear_error(&conv_error);
                g_free(normalized_data);
                if (combined) g_free(combined);
                
                if (progress_out) *progress_out = 1.0;
                return TRUE; /* Error */
            }
            
            /* Write to FD */
            const char *p_write = utf16_data;
            size_t rem = bytes_conv_written;
            while (rem > 0) {
                ssize_t w = write(task->fd, p_write, rem);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    task->error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                              "Write failed: %s", strerror(errno));
                    g_free(utf16_data);
                    g_free(normalized_data);
                    if (combined) g_free(combined);
                    if (progress_out) *progress_out = 1.0;
                    return TRUE;
                }
                p_write += w;
                rem -= w;
            }
            g_free(utf16_data);
            
        } else if (data_len > 0) {
            /* UTF-8 Direct Write */
            const char *p_write = data_to_encode;
            size_t rem = data_len;
             while (rem > 0) {
                ssize_t w = write(task->fd, p_write, rem);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    task->error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                              "Write failed: %s", strerror(errno));
                    g_free(normalized_data);
                    if (combined) g_free(combined);
                    if (progress_out) *progress_out = 1.0;
                    return TRUE;
                }
                p_write += w;
                rem -= w;
            }
        }
        
        g_free(normalized_data);
        if (combined) g_free(combined);
        
        task->bytes_written += bytes_to_process;
        piece_table_iter_advance(&task->iter, bytes_to_process);
    }
    
    if (progress_out) {
        *progress_out = task->total_bytes > 0 
                        ? (double)task->bytes_written / task->total_bytes 
                        : 1.0;
    }
    
    /* Check if done */
    return (piece_table_iter_get_chunk(&task->iter, NULL) == NULL);
}

GError *
piece_table_save_async_get_error(PieceTableSaveTask *task)
{
    if (task && task->error) {
        return g_error_copy(task->error);
    }
    return NULL;
}

gboolean
piece_table_save_async_had_lossy(PieceTableSaveTask *task)
{
    return task ? task->had_lossy_conversion : FALSE;
}

void
piece_table_save_async_finalize(PieceTableSaveTask *task)
{
    if (!task) return;
    
    /* fsync to ensure data is on disk */
    
    /* Handle pending bytes (invalid UTF-8 at end of file) */
    if (task->pending_len > 0) {
        /* Write as-is if no conversion needed, or if we want to preserve invalid bytes.
           If we are doing UTF-16, these bytes failed to form a char, so they are garbage?
           Sync implementation: writes them if !need_utf16.
        */
        gboolean need_conversion = (task->pt->encoding != ENCODING_UTF8);
        if (!need_conversion) {
             /* Before writing expected garbage, check saved_cr */
             if (task->saved_cr) {
                 const char *nl_str = "\n";
                 if (task->pt->newline_style == NEWLINE_CRLF) nl_str = "\r\n";
                 else if (task->pt->newline_style == NEWLINE_CR) nl_str = "\r";
                 write(task->fd, nl_str, strlen(nl_str));
                 task->saved_cr = FALSE;
             }
             
             size_t rem = task->pending_len;
             const char *p = task->pending_bytes;
             while (rem > 0) {
                 ssize_t w = write(task->fd, p, rem);
                 if (w > 0) {
                     p += w;
                     rem -= w;
                 } else if (w < 0 && errno != EINTR) {
                     /* Ignore error at this late stage or log? */
                     break;
                 }
             }
        }
    } else if (task->saved_cr) {
        /* If we have a pending CR and no more data, it's a trailing byte */
        const char *nl_str = "\n";
        if (task->pt->newline_style == NEWLINE_CRLF) nl_str = "\r\n";
        else if (task->pt->newline_style == NEWLINE_CR) nl_str = "\r";
        write(task->fd, nl_str, strlen(nl_str));
        task->saved_cr = FALSE;
    }
    
    fsync(task->fd);
    
    if (task->error) {
        g_error_free(task->error);
    }
    g_free(task);
}

void
piece_table_save_async_cancel(PieceTableSaveTask *task)
{
    if (task) {
        task->cancelled = TRUE;
    }
}
