#include <SYSTEM/FILES.H>

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#ifdef LBA2_TARGET_N64
// libdragon's newlib ships <utime.h> without the POSIX prototype; the no-op
// implementation lives in SYSTEM/N64_BACKEND.CPP.
struct utimbuf;
extern "C" int utime(const char *path, const struct utimbuf *times);
#endif

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// --- Private definitions -----------------------------------------------------
#ifndef _WIN32 // Portability - O_BINARY exists only on Windows platform
#define O_BINARY 0
#endif // _WIN32

#define MODE_READ (O_RDONLY | O_BINARY)
#define MODE_WRITE (O_WRONLY | O_BINARY | O_CREAT | O_TRUNC)
#define MODE_RW (O_RDWR | O_BINARY | O_CREAT)

// --- Private functions -------------------------------------------------------
S32 OpenMode(const char *filename, int mode) {
    assert(filename != NULL);
    S32 handle = open(filename, mode, (S_IRUSR | S_IWUSR));
    return (handle == -1) ? 0 : handle;
}

// --- Public functions --------------------------------------------------------
S32 OpenRead(const char *filename) { return OpenMode(filename, MODE_READ); }
S32 OpenWrite(const char *filename) { return OpenMode(filename, MODE_WRITE); }
S32 OpenReadWrite(const char *filename) { return OpenMode(filename, MODE_RW); }

S32 Close(S32 handle) {
    assert(handle > 0);
    return close(handle);
}

S32 Read(S32 handle, void *buffer, U32 maxReadBytes) {
    assert(handle > 0);
    assert(buffer != NULL);
    return read(handle, buffer, maxReadBytes);
}

S32 Write(S32 handle, const void *buffer, U32 maxWriteBytes) {
    assert(handle > 0);
    assert(buffer != NULL);
    return write(handle, buffer, maxWriteBytes);
}

S32 Seek(S32 handle, S32 offset, ESeekOrigin origin) {
    assert(handle > 0);

    int stdOrigin;
    switch (origin) {
    case SEEK_FROM_START:
        stdOrigin = SEEK_SET;
        break;
    case SEEK_FROM_CURRENT:
        stdOrigin = SEEK_CUR;
        break;
    case SEEK_FROM_END:
        stdOrigin = SEEK_END;
        break;
    default:
        assert(false);
        break;
    }

    return lseek(handle, offset, stdOrigin);
}

U32 Delete(const char *filename) {
    assert(filename != NULL);
    S32 result = unlink(filename);
    return (result != -1);
}

S32 Touch(const char *filename) {
    assert(filename != NULL);
    return utime(filename, NULL);
}

U32 FileSize(const char *filename) {
    assert(filename != NULL);

    S32 handle = OpenRead(filename);
    if (!handle) {
        return 0;
    }

    S32 fsize = lseek(handle, 0, SEEK_END);
    Close(handle);

    return (U32)((fsize == -1) ? 0 : fsize);
}

#ifdef LBA2_TARGET_N64
// libdragon's rom:/ devoptab has patchy stat() support (directories and
// trailing separators in particular). Probe with stat first, then fall back
// to fopen (files) and dir_findfirst (directories, which also tolerates the
// empty-dir -1 return).
#include <SYSTEM/LIMITS.H> // ADELINE_MAX_PATH

#include <dir.h>
#include <stdio.h>
#include <string.h>

static bool N64DirExists(const char *path) {
    char buf[ADELINE_MAX_PATH];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, path, n + 1);
    // Strip trailing separators, but keep the one of a bare "rom:/" root.
    while (n > 1 && buf[n - 1] == '/' && buf[n - 2] != ':') buf[--n] = '\0';
    dir_t d;
    // Only a clean 0 counts: DFS error codes overlap with the "-1 = empty
    // dir" convention and produced false positives (every probed path
    // "existed", so discovery settled on the wrong resource dir). An empty
    // dir cannot occur in a DFS image anyway.
    return dir_findfirst(buf, &d) == 0;
}
#endif

bool ExistsFileOrDir(const char *path) {
    assert(path != NULL);

    struct stat status = {};
    if (stat(path, &status) == 0) return true;
#ifdef LBA2_TARGET_N64
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return N64DirExists(path);
#else
    return false;
#endif
}

bool IsDirectory(const char *path) {
    assert(path != NULL);

    struct stat status = {};
    if (stat(path, &status) == 0) return ((status.st_mode & S_IFMT) == S_IFDIR);
#ifdef LBA2_TARGET_N64
    return N64DirExists(path);
#else
    return false;
#endif
}

// =============================================================================
#ifdef __cplusplus
}
#endif
