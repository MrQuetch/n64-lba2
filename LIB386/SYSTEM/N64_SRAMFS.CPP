// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Cartridge SRAM filesystem for the Nintendo 64 port ("sram:/" devoptab).
//
// The engine addresses saves as ordinary files (<player>.LBA, autosave.lba,
// current.lba under the user dir's save/ folder, plus lba2.cfg at its root)
// through POSIX open/read/write/stat/unlink and the dirent shim. A cartridge
// has no disk, only the 32 KB of battery-backed SRAM declared in the ROM
// header, so this module presents that SRAM as a tiny flat filesystem:
//
//   image (32768 bytes, mirrored in RDRAM)
//   +--------+------------------------------------------------------------+
//   | header | record | record | ...                        (free space)   |
//   +--------+------------------------------------------------------------+
//   header : "LBA2" magic, format version, file count, used bytes, CRC32
//   record : U16 data size, U8 name length, name, data   (no padding)
//
// Names keep their directory part ("save/TWINSEN.LBA", "lba2.cfg"); the
// directories themselves are virtual. Lookups are case-insensitive, matching
// the DOS/Windows semantics the 1997 engine was written against (it probes
// case variations of every path). Reads are served from the RDRAM mirror;
// a write is buffered per handle and committed on close(), when the whole
// image is rebuilt and DMA'd back to the cartridge — one PI transfer of
// 32 KB, a few milliseconds, on every save.
//
// A blank or corrupt part (CRC mismatch: fresh flashcart, different game's
// leftovers) is formatted silently. Emulators persist the image as a .sav
// next to the ROM; flashcarts write it back to SD on reset/power-off.

#include <SYSTEM/N64_SRAMFS.H>

#include <SYSTEM/LOGPRINT.H>

#include <libdragon.h>
// filesystem_t / attach_filesystem live in libdragon's <system.h>, which the
// engine's own LIB386/H/SYSTEM.H shadows on case-insensitive hosts (Windows
// Docker mounts). Reach it relative to the toolchain include dir instead.
#include <../include/system.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Layout -------------------------------------------------------------------
#define SRAM_PI_BASE 0x08000000UL // cartridge domain 2 (SRAM/FlashRAM)
#define SRAMFS_MAGIC "LBA2"
#define SRAMFS_FORMAT_VERSION 1
#define SRAMFS_HEADER_SIZE 16
#define SRAMFS_MAX_FILES 16
#define SRAMFS_MAX_NAME 200 // U8 on disk; PlayerName (100) + ".LBA" + "save/"
#define SRAMFS_MAX_HANDLES 8
#define SRAMFS_RECORD_OVERHEAD 3 // U16 size + U8 namelen

// PI registers (KSEG1). DOM2 timings are the ones every SRAM title programs
// (libultra's SRAM driver values); the reset defaults are not guaranteed.
#define PI_STATUS_REG (*(volatile uint32_t *)0xA4600010)
#define PI_BSD_DOM2_LAT_REG (*(volatile uint32_t *)0xA4600024)
#define PI_BSD_DOM2_PWD_REG (*(volatile uint32_t *)0xA4600028)
#define PI_BSD_DOM2_PGS_REG (*(volatile uint32_t *)0xA460002C)
#define PI_BSD_DOM2_RLS_REG (*(volatile uint32_t *)0xA4600030)

typedef struct {
    char name[SRAMFS_MAX_NAME + 1]; // "save/NAME.LBA" or "lba2.cfg"
    uint32_t size;
    uint8_t *data; // malloc'd copy, owned by the entry
} SramEntry;

typedef struct {
    int used;
    int writable;
    int dirty;
    int failed; // a write hit ENOSPC: close() discards instead of committing
    char name[SRAMFS_MAX_NAME + 1];
    uint8_t *buf; // private copy (read) or pending content (write)
    uint32_t size;
    uint32_t cap;
    uint32_t pos;
} SramHandle;

static uint8_t s_image[N64_SRAMFS_SIZE] __attribute__((aligned(16)));
static SramEntry s_entries[SRAMFS_MAX_FILES];
static int s_numEntries = 0;
static SramHandle s_handles[SRAMFS_MAX_HANDLES];
static uint32_t s_crcTable[256];
static int s_ready = 0;

// --- CRC32 (reflected, poly 0xEDB88320) ---------------------------------------
static void CrcInit(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        s_crcTable[i] = c;
    }
}

static uint32_t Crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFUL;
    while (n--) c = s_crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

// --- Raw SRAM access ----------------------------------------------------------
static void SramSetTimings(void) {
    while (PI_STATUS_REG & 3) {}
    PI_BSD_DOM2_LAT_REG = 0x05;
    PI_BSD_DOM2_PWD_REG = 0x0C;
    PI_BSD_DOM2_PGS_REG = 0x0D;
    PI_BSD_DOM2_RLS_REG = 0x02;
}

static void SramReadAll(void) {
    SramSetTimings();
    data_cache_hit_writeback_invalidate(s_image, N64_SRAMFS_SIZE);
    dma_read_raw_async(s_image, SRAM_PI_BASE, N64_SRAMFS_SIZE);
    dma_wait();
    data_cache_hit_invalidate(s_image, N64_SRAMFS_SIZE);
}

static void SramWriteAll(void) {
    SramSetTimings();
    data_cache_hit_writeback(s_image, N64_SRAMFS_SIZE);
    dma_write_raw_async(s_image, SRAM_PI_BASE, N64_SRAMFS_SIZE);
    dma_wait();
}

// --- Image <-> directory ------------------------------------------------------
static inline void Put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void Put32(uint8_t *p, uint32_t v) {
    Put16(p, v >> 16);
    Put16(p + 2, v & 0xFFFF);
}
static inline uint32_t Get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t Get32(const uint8_t *p) { return (Get16(p) << 16) | Get16(p + 2); }

static uint32_t RecordBytes(const SramEntry *e) {
    return SRAMFS_RECORD_OVERHEAD + (uint32_t)strlen(e->name) + e->size;
}

unsigned N64SramFs_UsedBytes(void) {
    uint32_t used = SRAMFS_HEADER_SIZE;
    for (int i = 0; i < s_numEntries; i++) used += RecordBytes(&s_entries[i]);
    return used;
}

static void FreeEntries(void) {
    for (int i = 0; i < s_numEntries; i++) {
        free(s_entries[i].data);
        s_entries[i].data = NULL;
    }
    s_numEntries = 0;
}

// Parse the RDRAM mirror into the entry table. Returns 0 when the image is a
// valid filesystem, -1 when it must be formatted.
static int ParseImage(void) {
    FreeEntries();

    const uint8_t *h = s_image;
    if (memcmp(h, SRAMFS_MAGIC, 4) != 0) return -1;
    if (h[4] != SRAMFS_FORMAT_VERSION) return -1;
    uint32_t nfiles = h[5];
    uint32_t used = Get32(h + 8);
    uint32_t crc = Get32(h + 12);
    if (nfiles > SRAMFS_MAX_FILES) return -1;
    if (used > N64_SRAMFS_SIZE - SRAMFS_HEADER_SIZE) return -1;
    if (Crc32(s_image + SRAMFS_HEADER_SIZE, used) != crc) return -1;

    const uint8_t *p = s_image + SRAMFS_HEADER_SIZE;
    const uint8_t *end = p + used;
    for (uint32_t i = 0; i < nfiles; i++) {
        if (end - p < SRAMFS_RECORD_OVERHEAD) goto corrupt;
        uint32_t size = Get16(p);
        uint32_t namelen = p[2];
        p += SRAMFS_RECORD_OVERHEAD;
        if (namelen == 0 || namelen > SRAMFS_MAX_NAME) goto corrupt;
        if ((uint32_t)(end - p) < namelen + size) goto corrupt;

        SramEntry *e = &s_entries[s_numEntries];
        memcpy(e->name, p, namelen);
        e->name[namelen] = '\0';
        p += namelen;
        e->size = size;
        e->data = (uint8_t *)malloc(size ? size : 1);
        if (!e->data) goto corrupt;
        memcpy(e->data, p, size);
        p += size;
        s_numEntries++;
    }
    if (p != end) goto corrupt;
    return 0;

corrupt:
    FreeEntries();
    return -1;
}

// Rebuild the image from the entry table and push it to the cartridge.
static void Commit(void) {
    memset(s_image, 0, N64_SRAMFS_SIZE);
    uint8_t *p = s_image + SRAMFS_HEADER_SIZE;
    for (int i = 0; i < s_numEntries; i++) {
        const SramEntry *e = &s_entries[i];
        uint32_t namelen = (uint32_t)strlen(e->name);
        Put16(p, e->size);
        p[2] = (uint8_t)namelen;
        p += SRAMFS_RECORD_OVERHEAD;
        memcpy(p, e->name, namelen);
        p += namelen;
        memcpy(p, e->data, e->size);
        p += e->size;
    }
    uint32_t used = (uint32_t)(p - (s_image + SRAMFS_HEADER_SIZE));

    memcpy(s_image, SRAMFS_MAGIC, 4);
    s_image[4] = SRAMFS_FORMAT_VERSION;
    s_image[5] = (uint8_t)s_numEntries;
    Put16(s_image + 6, 0);
    Put32(s_image + 8, used);
    Put32(s_image + 12, Crc32(s_image + SRAMFS_HEADER_SIZE, used));

    SramWriteAll();
}

// --- Path helpers -------------------------------------------------------------
// libdragon hands open()/unlink() the path without the leading slash and
// stat()/findfirst()/mkdir() with it; normalise to "a/b" with no leading or
// trailing separators.
static void NormalizePath(const char *in, char *out, size_t outsize) {
    while (*in == '/') in++;
    size_t n = strlen(in);
    while (n > 0 && in[n - 1] == '/') n--;
    if (n >= outsize) n = outsize - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

static int FindEntry(const char *name) {
    for (int i = 0; i < s_numEntries; i++)
        if (strcasecmp(s_entries[i].name, name) == 0) return i;
    return -1;
}

// The engine expects its save tree (save/, save/shoot/) to exist; any other
// directory exists as soon as a file lives under it.
static int PathIsDir(const char *path) {
    if (path[0] == '\0') return 1;
    if (strcasecmp(path, "save") == 0 || strcasecmp(path, "save/shoot") == 0) return 1;
    size_t n = strlen(path);
    for (int i = 0; i < s_numEntries; i++) {
        const char *nm = s_entries[i].name;
        if (strncasecmp(nm, path, n) == 0 && nm[n] == '/') return 1;
    }
    return 0;
}

// Is entry `i` a direct child of directory `dir`? Returns its basename.
static const char *ChildOf(int i, const char *dir) {
    const char *nm = s_entries[i].name;
    size_t n = strlen(dir);
    if (n > 0) {
        if (strncasecmp(nm, dir, n) != 0 || nm[n] != '/') return NULL;
        nm += n + 1;
    }
    return strchr(nm, '/') ? NULL : nm;
}

// --- Handles ------------------------------------------------------------------
static SramHandle *AllocHandle(void) {
    for (int i = 0; i < SRAMFS_MAX_HANDLES; i++)
        if (!s_handles[i].used) {
            memset(&s_handles[i], 0, sizeof(s_handles[i]));
            s_handles[i].used = 1;
            return &s_handles[i];
        }
    return NULL;
}

static void ReleaseHandle(SramHandle *h) {
    free(h->buf);
    memset(h, 0, sizeof(*h));
}

// Space this handle may grow to: everything not taken by the other files.
static uint32_t HandleBudget(const SramHandle *h) {
    uint32_t used = SRAMFS_HEADER_SIZE;
    for (int i = 0; i < s_numEntries; i++) {
        if (strcasecmp(s_entries[i].name, h->name) == 0) continue;
        used += RecordBytes(&s_entries[i]);
    }
    uint32_t overhead = SRAMFS_RECORD_OVERHEAD + (uint32_t)strlen(h->name);
    if (used + overhead >= N64_SRAMFS_SIZE) return 0;
    return N64_SRAMFS_SIZE - used - overhead;
}

static int HandleReserve(SramHandle *h, uint32_t size) {
    if (size <= h->cap) return 0;
    uint32_t cap = h->cap ? h->cap : 1024;
    while (cap < size) cap *= 2;
    if (cap > N64_SRAMFS_SIZE) cap = N64_SRAMFS_SIZE;
    uint8_t *nb = (uint8_t *)realloc(h->buf, cap);
    if (!nb) return -1;
    h->buf = nb;
    h->cap = cap;
    return 0;
}

// --- devoptab callbacks -------------------------------------------------------
static void *SramOpen(char *name, int flags) {
    char path[SRAMFS_MAX_NAME + 1];
    if (strlen(name) > SRAMFS_MAX_NAME) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    NormalizePath(name, path, sizeof(path));
    if (path[0] == '\0' || PathIsDir(path)) {
        errno = EISDIR;
        return NULL;
    }

    int acc = flags & O_ACCMODE;
    int writable = (acc == O_WRONLY || acc == O_RDWR);
    int idx = FindEntry(path);
    if (idx < 0 && !(writable && (flags & O_CREAT))) {
        errno = ENOENT;
        return NULL;
    }
    if (s_numEntries >= SRAMFS_MAX_FILES && idx < 0) {
        errno = ENOSPC;
        return NULL;
    }

    SramHandle *h = AllocHandle();
    if (!h) {
        errno = EMFILE;
        return NULL;
    }
    // Keep the stored spelling when overwriting an existing file, so a
    // "twinsen.lba" probe hits the same slot as "TWINSEN.LBA".
    strcpy(h->name, idx >= 0 ? s_entries[idx].name : path);
    h->writable = writable;

    int keep = (idx >= 0) && !(writable && (flags & O_TRUNC));
    if (keep) {
        const SramEntry *e = &s_entries[idx];
        if (HandleReserve(h, e->size) != 0) {
            ReleaseHandle(h);
            errno = ENOMEM;
            return NULL;
        }
        memcpy(h->buf, e->data, e->size);
        h->size = e->size;
    } else if (writable) {
        h->dirty = 1; // create/truncate: even an empty close() writes a record
    }
    h->pos = (flags & O_APPEND) ? h->size : 0;
    return h;
}

static int SramFstat(void *file, struct stat *st) {
    SramHandle *h = (SramHandle *)file;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = (off_t)h->size;
    return 0;
}

static int SramStat(char *name, struct stat *st) {
    char path[SRAMFS_MAX_NAME + 1];
    NormalizePath(name, path, sizeof(path));
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    if (PathIsDir(path)) {
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    int idx = FindEntry(path);
    if (idx < 0) {
        errno = ENOENT;
        return -1;
    }
    st->st_mode = S_IFREG | 0644;
    st->st_size = (off_t)s_entries[idx].size;
    return 0;
}

static int SramLseek(void *file, int ptr, int dir) {
    SramHandle *h = (SramHandle *)file;
    int base;
    switch (dir) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int)h->pos; break;
    case SEEK_END: base = (int)h->size; break;
    default:
        errno = EINVAL;
        return -1;
    }
    int np = base + ptr;
    if (np < 0 || np > N64_SRAMFS_SIZE) {
        errno = EINVAL;
        return -1;
    }
    h->pos = (uint32_t)np;
    return np;
}

static int SramRead(void *file, uint8_t *ptr, int len) {
    SramHandle *h = (SramHandle *)file;
    if (len < 0) {
        errno = EINVAL;
        return -1;
    }
    if (h->pos >= h->size) return 0;
    uint32_t n = h->size - h->pos;
    if ((uint32_t)len < n) n = (uint32_t)len;
    memcpy(ptr, h->buf + h->pos, n);
    h->pos += n;
    return (int)n;
}

static int SramWrite(void *file, uint8_t *ptr, int len) {
    SramHandle *h = (SramHandle *)file;
    if (!h->writable) {
        errno = EBADF;
        return -1;
    }
    if (len < 0) {
        errno = EINVAL;
        return -1;
    }
    if (len == 0) return 0;

    uint32_t end = h->pos + (uint32_t)len;
    if (end > h->size && end > HandleBudget(h)) {
        LogPrintf("[sram] '%s': no room for %lu bytes (used %u of %d)\n", h->name,
                  (unsigned long)end, N64SramFs_UsedBytes(), N64_SRAMFS_SIZE);
        h->failed = 1;
        errno = ENOSPC;
        return -1;
    }
    if (HandleReserve(h, end) != 0) {
        errno = ENOMEM;
        return -1;
    }
    if (h->pos > h->size) memset(h->buf + h->size, 0, h->pos - h->size); // sparse gap
    memcpy(h->buf + h->pos, ptr, (size_t)len);
    h->pos = end;
    if (end > h->size) h->size = end;
    h->dirty = 1;
    return len;
}

static int SramClose(void *file) {
    SramHandle *h = (SramHandle *)file;
    // A save that ran out of room is dropped whole rather than committed as
    // a truncated record: the engine only checks FileSize() afterwards, and
    // a 0-byte .LBA would show up in the load menu as a bogus slot.
    if (h->dirty && !h->failed) {
        int idx = FindEntry(h->name);
        if (idx < 0) {
            if (s_numEntries >= SRAMFS_MAX_FILES) {
                ReleaseHandle(h);
                errno = ENOSPC;
                return -1;
            }
            idx = s_numEntries++;
            strcpy(s_entries[idx].name, h->name);
            s_entries[idx].data = NULL;
        }
        SramEntry *e = &s_entries[idx];
        free(e->data);
        e->data = h->buf ? h->buf : (uint8_t *)malloc(1);
        e->size = h->size;
        h->buf = NULL; // ownership moved to the entry
        Commit();
        LogPrintf("[sram] wrote '%s' (%lu bytes), %u of %d bytes in use\n", e->name,
                  (unsigned long)e->size, N64SramFs_UsedBytes(), N64_SRAMFS_SIZE);
    }
    ReleaseHandle(h);
    return 0;
}

static int SramUnlink(char *name) {
    char path[SRAMFS_MAX_NAME + 1];
    NormalizePath(name, path, sizeof(path));
    int idx = FindEntry(path);
    if (idx < 0) {
        errno = ENOENT;
        return -1;
    }
    free(s_entries[idx].data);
    for (int i = idx + 1; i < s_numEntries; i++) s_entries[i - 1] = s_entries[i];
    s_numEntries--;
    memset(&s_entries[s_numEntries], 0, sizeof(s_entries[0]));
    Commit();
    LogPrintf("[sram] deleted '%s', %u of %d bytes in use\n", path, N64SramFs_UsedBytes(),
              N64_SRAMFS_SIZE);
    return 0;
}

// Directory walk: d_cookie holds the index of the next entry to examine.
static int FindFrom(const char *dirpath, int start, dir_t *dir) {
    for (int i = start; i < s_numEntries; i++) {
        const char *base = ChildOf(i, dirpath);
        if (!base) continue;
        strncpy(dir->d_name, base, sizeof(dir->d_name) - 1);
        dir->d_name[sizeof(dir->d_name) - 1] = '\0';
        dir->d_type = DT_REG;
        dir->d_size = s_entries[i].size;
        dir->d_cookie = (uint32_t)(i + 1);
        return 0;
    }
    return -1; // end of listing (also "directory is empty")
}

static int SramFindFirst(char *path, dir_t *dir) {
    char dirpath[SRAMFS_MAX_NAME + 1];
    NormalizePath(path, dirpath, sizeof(dirpath));
    if (!PathIsDir(dirpath)) {
        errno = ENOENT;
        return -2;
    }
    return FindFrom(dirpath, 0, dir);
}

static int SramFindNext(const char *path, dir_t *dir) {
    char dirpath[SRAMFS_MAX_NAME + 1];
    NormalizePath(path, dirpath, sizeof(dirpath));
    return FindFrom(dirpath, (int)dir->d_cookie, dir);
}

static int SramMkdir(char *path, mode_t mode) {
    // Directories are implicit; report success so AddDirIfNot() is happy.
    (void)path;
    (void)mode;
    return 0;
}

static filesystem_t s_fs;

// --- Init ---------------------------------------------------------------------
int N64SramFs_Init(void) {
    if (s_ready) return 0;
    CrcInit();
    memset(s_handles, 0, sizeof(s_handles));

    SramReadAll();
    if (ParseImage() != 0) {
        LogPrintf("[sram] no valid filesystem in SRAM: formatting\n");
        FreeEntries();
        Commit();
    }

    memset(&s_fs, 0, sizeof(s_fs));
    s_fs.open = SramOpen;
    s_fs.fstat = SramFstat;
    s_fs.stat = SramStat;
    s_fs.lseek = SramLseek;
    s_fs.read = SramRead;
    s_fs.write = SramWrite;
    s_fs.close = SramClose;
    s_fs.unlink = SramUnlink;
    s_fs.findfirst = SramFindFirst;
    s_fs.findnext2 = SramFindNext;
    s_fs.mkdir = SramMkdir;

    int rc = attach_filesystem(N64_SRAMFS_PREFIX, &s_fs);
    if (rc != 0) {
        LogPrintf("[sram] attach_filesystem failed (%d)\n", rc);
        return rc;
    }
    s_ready = 1;

    LogPrintf("[sram] mounted %s: %d file(s), %u of %d bytes in use\n", N64_SRAMFS_PREFIX,
              s_numEntries, N64SramFs_UsedBytes(), N64_SRAMFS_SIZE);
    for (int i = 0; i < s_numEntries; i++)
        LogPrintf("[sram]   %-24s %6lu bytes\n", s_entries[i].name,
                  (unsigned long)s_entries[i].size);
    return 0;
}

#ifdef __cplusplus
}
#endif
