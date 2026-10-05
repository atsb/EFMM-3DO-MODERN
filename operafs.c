#if !defined(_MSC_VER)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "operafs.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#if !defined(_MSC_VER)
#include <sys/types.h>
#endif

#define OPERA_BLOCK_SIZE 2048u
#define OPERA_NAME_MAX 32
#define OPERA_DIR_HEADER_SIZE 20u
#define OPERA_DIRENT_FIXED_SIZE 0x44u
#define OPERA_MAX_ROOT_COPIES 8u
#define OPERA_MAX_DIR_COPY 16u
#define OPERA_DIRENT_FILE 0x00000002u
#define OPERA_DIRENT_SPECIAL 0x00000006u
#define OPERA_DIRENT_DIR 0x00000007u
#define OPERA_DIRENT_TYPE_MASK 0x000000FFu
#define OPERA_LAST_DIRENT_IN_BLOCK 0x40000000u
#define OPERA_LAST_DIRENT_IN_DIR 0x80000000u
#define OPERA_END_BLOCK 0xFFFFFFFFu
#define OPERA_MAX_PATH 1024
#define OPERA_PROBE_SECTORS 300u

typedef enum OperaSectorFormat
{
    OPERA_SECT_DATA_2048 = 0,
    OPERA_SECT_RAW_2352,
    OPERA_SECT_RAW_2448,
    OPERA_SECT_RAW_2336
} OperaSectorFormat;

typedef struct OperaDirRef
{
    uint32_t start;
    uint32_t blocks;
} OperaDirRef;

struct OperaFS
{
    FILE *image;
    char imagePath[OPERA_MAX_PATH];
    char cwd[OPERA_MAX_PATH];
    OperaSectorFormat sectorFormat;
    uint32_t blockSize;
    uint32_t blockCount;
    int64_t dataOffset;
    OperaDirRef root;
};

struct OperaFile
{
    uint32_t startBlock;
    uint32_t blockCount;
    uint32_t byteCount;
    int64_t position;
};

static OperaFS gOpera;

static uint32_t opera_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int opera_ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static int opera_name_equal(const unsigned char *entryName, const char *name)
{
    size_t i;
    if (!entryName || !name)
        return 0;
    for (i = 0; i < OPERA_NAME_MAX; ++i)
    {
        int a = entryName[i];
        int b = (unsigned char)name[i];
        if (b == 0)
            return a == 0 || a == ' ';
        if (a == 0)
            return 0;
        if (opera_ascii_tolower(a) != opera_ascii_tolower(b))
            return 0;
    }
    return name[OPERA_NAME_MAX] == 0;
}

static void opera_normalize_separators(char *path)
{
    char *p;
    for (p = path; p && *p; ++p)
    {
        if (*p == '\\')
            *p = '/';
    }
}

static int opera_is_root_token(const char *path)
{
    if (!path)
        return 0;
    if (!strcmp(path, "$progdir") || !strcmp(path, "$progdir/"))
        return 1;
    if (!strcmp(path, "/") || !strcmp(path, ""))
        return 1;
    return 0;
}

static int64_t opera_file_seek(FILE *fp, int64_t offset, int whence)
{
#if defined(_MSC_VER)
    if (_fseeki64(fp, offset, whence) != 0)
        return -1;
    return _ftelli64(fp);
#else
    if (fseeko(fp, (off_t)offset, whence) != 0)
        return -1;
    return (int64_t)ftello(fp);
#endif
}

static int64_t opera_sector_stride(OperaSectorFormat fmt)
{
    switch (fmt)
    {
    case OPERA_SECT_RAW_2352:
        return 2352;
    case OPERA_SECT_RAW_2448:
        return 2448;
    case OPERA_SECT_RAW_2336:
        return 2336;
    default:
        return 2048;
    }
}

static int64_t opera_sector_payload(OperaSectorFormat fmt)
{
    switch (fmt)
    {
    case OPERA_SECT_RAW_2352:
    case OPERA_SECT_RAW_2448:
        return 16;
    case OPERA_SECT_RAW_2336:
        return 8;
    default:
        return 0;
    }
}

static int opera_read_block_raw(OperaFS *fs, uint32_t block, void *buffer)
{
    int64_t offset;
    size_t got;
    if (!fs || !fs->image || !buffer || block >= fs->blockCount)
        return 0;
    offset = fs->dataOffset +
             (int64_t)block * opera_sector_stride(fs->sectorFormat) +
             opera_sector_payload(fs->sectorFormat);
    if (opera_file_seek(fs->image, offset, SEEK_SET) < 0)
        return 0;
    got = fread(buffer, 1, OPERA_BLOCK_SIZE, fs->image);
    return got == OPERA_BLOCK_SIZE;
}

static int opera_valid_superblock(const unsigned char *b, uint64_t fileSize, int64_t dataOffset,
                                  OperaSectorFormat fmt, uint32_t *blockCountOut,
                                  OperaDirRef *rootOut)
{
    uint32_t blockSize, blockCount, rootBlocks, rootBlockSize, lastCopy;
    uint64_t needBytes;
    uint32_t i, rootStart = 0;

    if (b[0] != 1)
        return 0;
    if (b[1] != 0x5A || b[2] != 0x5A || b[3] != 0x5A || b[4] != 0x5A || b[5] != 0x5A)
        return 0;
    if (b[6] != 1)
        return 0;

    /* The 3DO volume label occupies 0x28..0x47. */
    if (memcmp(b + 0x28, "CD-ROM", 6) != 0)
        return 0;

    blockSize = opera_be32(b + 0x4C);
    blockCount = opera_be32(b + 0x50);
    if (blockSize != OPERA_BLOCK_SIZE || blockCount == 0)
        return 0;

    needBytes = (uint64_t)dataOffset + (uint64_t)blockCount * opera_sector_stride(fmt);
    if (needBytes > fileSize)
    {
        uint64_t lastByte = (uint64_t)dataOffset +
                            (uint64_t)(blockCount - 1) * opera_sector_stride(fmt) +
                            (uint64_t)opera_sector_payload(fmt) + OPERA_BLOCK_SIZE;
        if (lastByte > fileSize)
            return 0;
    }

    rootBlocks = opera_be32(b + 0x58);
    rootBlockSize = opera_be32(b + 0x5C);
    lastCopy = opera_be32(b + 0x60);
    if (rootBlocks == 0 || rootBlockSize != OPERA_BLOCK_SIZE || lastCopy >= OPERA_MAX_ROOT_COPIES)
        return 0;

    for (i = 0; i <= lastCopy; ++i)
    {
        uint32_t candidate = opera_be32(b + 0x64 + i * 4);
        if (candidate < blockCount)
        {
            rootStart = candidate;
            break;
        }
    }
    if (!rootStart)
        return 0;

    if (blockCountOut)
        *blockCountOut = blockCount;
    if (rootOut)
    {
        rootOut->start = rootStart;
        rootOut->blocks = rootBlocks;
    }
    return 1;
}

static int opera_get_file_size(FILE *fp, uint64_t *sizeOut)
{
    int64_t end;
    if (opera_file_seek(fp, 0, SEEK_END) < 0)
        return 0;
    end = opera_file_seek(fp, 0, SEEK_CUR);
    if (end < 0)
        return 0;
    if (sizeOut)
        *sizeOut = (uint64_t)end;
    return 1;
}

static int opera_detect_format(FILE *fp, OperaSectorFormat *fmtOut,
                               uint32_t *blockCountOut, OperaDirRef *rootOut,
                               int64_t *dataOffsetOut)
{
    unsigned char block[OPERA_BLOCK_SIZE];
    uint64_t fileSize = 0;
    const OperaSectorFormat formats[] = {
        OPERA_SECT_DATA_2048,
        OPERA_SECT_RAW_2352,
        OPERA_SECT_RAW_2448,
        OPERA_SECT_RAW_2336};
    size_t i;

    if (!opera_get_file_size(fp, &fileSize))
        return 0;
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
    {
        uint32_t probe;
        int64_t stride = opera_sector_stride(formats[i]);
        for (probe = 0; probe <= OPERA_PROBE_SECTORS; ++probe)
        {
            int64_t dataOffset = (int64_t)probe * stride;
            int64_t pos = dataOffset + opera_sector_payload(formats[i]);
            if ((uint64_t)pos + OPERA_BLOCK_SIZE > fileSize)
                break;
            if (opera_file_seek(fp, pos, SEEK_SET) < 0)
                continue;
            if (fread(block, 1, OPERA_BLOCK_SIZE, fp) != OPERA_BLOCK_SIZE)
                continue;
            {
                uint32_t count;
                OperaDirRef root;
                if (opera_valid_superblock(block, fileSize, dataOffset, formats[i], &count, &root))
                {
                    if (fmtOut)
                        *fmtOut = formats[i];
                    if (blockCountOut)
                        *blockCountOut = count;
                    if (rootOut)
                        *rootOut = root;
                    if (dataOffsetOut)
                        *dataOffsetOut = dataOffset;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int opera_dir_next_block(OperaFS *fs, uint32_t dirStart, uint32_t dirBlocks,
                                uint32_t current, uint32_t encodedNext, uint32_t *nextOut)
{
    unsigned char block[OPERA_BLOCK_SIZE];
    uint32_t candidates[3];
    size_t i;

    if (!nextOut || encodedNext == OPERA_END_BLOCK)
        return 0;

    candidates[0] = dirStart + encodedNext;
    candidates[1] = encodedNext;
    candidates[2] = current + 1;

    for (i = 0; i < 3; ++i)
    {
        uint32_t c = candidates[i];
        uint32_t prev;
        if (c >= fs->blockCount)
            continue;
        if (c < dirStart || c >= dirStart + dirBlocks)
            continue;
        if (c == current)
            continue;
        if (!opera_read_block_raw(fs, c, block))
            continue;
        prev = opera_be32(block + 4);
        if (prev == OPERA_END_BLOCK || prev == current ||
            prev == current - dirStart || c == current + 1)
        {
            *nextOut = c;
            return 1;
        }
    }
    return 0;
}

static int opera_dir_find_entry(OperaFS *fs, OperaDirRef dir,
                                const char *name, uint32_t *flagsOut,
                                uint32_t *startOut, uint32_t *blocksOut,
                                uint32_t *bytesOut, char *entryNameOut)
{
    unsigned char block[OPERA_BLOCK_SIZE];
    uint32_t current;
    uint32_t visited = 0;

    if (!fs || dir.blocks == 0 || dir.start >= fs->blockCount || !name)
        return 0;
    current = dir.start;

    while (visited < dir.blocks + 2 && current < fs->blockCount)
    {
        uint32_t firstFree, firstEntry, off;
        uint32_t nextEncoded, nextBlock;
        int lastDir = 0;

        if (!opera_read_block_raw(fs, current, block))
            return 0;

        firstFree = opera_be32(block + 12);
        firstEntry = opera_be32(block + 16);
        if (firstEntry < OPERA_DIR_HEADER_SIZE || firstEntry >= OPERA_BLOCK_SIZE)
            firstEntry = OPERA_DIR_HEADER_SIZE;
        if (firstFree < firstEntry || firstFree > OPERA_BLOCK_SIZE)
            firstFree = OPERA_BLOCK_SIZE;

        off = firstEntry;
        while (off + OPERA_DIRENT_FIXED_SIZE <= firstFree)
        {
            uint32_t flags = opera_be32(block + off);
            uint32_t lastCopy = opera_be32(block + off + 0x40);
            uint32_t entrySize;
            const unsigned char *entryName = block + off + 0x20;
            const unsigned char *copies;

            if (lastCopy >= OPERA_MAX_DIR_COPY)
                break;
            entrySize = OPERA_DIRENT_FIXED_SIZE + 4u * (lastCopy + 1u);
            if (entrySize < OPERA_DIRENT_FIXED_SIZE || off + entrySize > firstFree ||
                off + entrySize > OPERA_BLOCK_SIZE)
                break;
            copies = block + off + OPERA_DIRENT_FIXED_SIZE;

            if (opera_name_equal(entryName, name))
            {
                uint32_t type = flags & OPERA_DIRENT_TYPE_MASK;
                if (entryNameOut)
                {
                    size_t n = 0;
                    while (n < OPERA_NAME_MAX && entryName[n])
                    {
                        entryNameOut[n] = (char)entryName[n];
                        ++n;
                    }
                    entryNameOut[n] = 0;
                }
                if (flagsOut)
                    *flagsOut = flags;
                if (type == OPERA_DIRENT_FILE || type == OPERA_DIRENT_SPECIAL || type == OPERA_DIRENT_DIR)
                {
                    if (startOut)
                        *startOut = opera_be32(copies);
                    if (blocksOut)
                        *blocksOut = opera_be32(block + off + 0x14);
                    if (bytesOut)
                        *bytesOut = opera_be32(block + off + 0x10);
                    return 1;
                }
            }

            off += entrySize;
            if (flags & OPERA_LAST_DIRENT_IN_DIR)
            {
                lastDir = 1;
                break;
            }
            if (flags & OPERA_LAST_DIRENT_IN_BLOCK)
                break;
        }

        if (lastDir)
            break;
        nextEncoded = opera_be32(block + 0);
        if (!opera_dir_next_block(fs, dir.start, dir.blocks, current, nextEncoded, &nextBlock))
            break;
        current = nextBlock;
        ++visited;
    }
    return 0;
}

static int opera_split_next(const char **cursor, char *component, size_t cap)
{
    const char *p = *cursor;
    size_t n = 0;
    while (*p == '/')
        ++p;
    if (!*p)
    {
        *cursor = p;
        return 0;
    }
    while (*p && *p != '/')
    {
        if (n + 1 < cap)
            component[n++] = *p;
        ++p;
    }
    component[n] = 0;
    *cursor = p;
    return n != 0;
}

static int opera_lookup_path(const char *path, uint32_t *flagsOut,
                             uint32_t *startOut, uint32_t *blocksOut,
                             uint32_t *bytesOut)
{
    char local[OPERA_MAX_PATH];
    char component[OPERA_NAME_MAX + 1];
    const char *cursor;
    OperaDirRef dir;
    uint32_t flags, start, blocks, bytes;

    if (!gOpera.image || !path)
        return 0;
    if (strlen(path) >= sizeof(local))
        return 0;
    strcpy(local, path);
    opera_normalize_separators(local);

    if (local[0] == '$' && !strncmp(local, "$progdir", 8))
    {
        const char *p = local + 8;
        if (*p == '/')
            ++p;
        memmove(local, p, strlen(p) + 1);
        local[0] = local[0] ? local[0] : '\0';
        cursor = local;
        dir = gOpera.root;
    }
    else if (local[0] == '^' && (local[1] == '/' || local[1] == '\0'))
    {
        /* 3DO File Folio uses ^/ as a root-relative path. */
        const char *p = local + 1;
        if (*p == '/')
            ++p;
        memmove(local, p, strlen(p) + 1);
        cursor = local;
        dir = gOpera.root;
    }
    else if (local[0] == '/')
    {
        cursor = local;
        dir = gOpera.root;
    }
    else
    {
        char cwdCopy[OPERA_MAX_PATH];
        if (strlen(gOpera.cwd) + strlen(local) + 2 >= sizeof(cwdCopy))
            return 0;
        if (!strcmp(gOpera.cwd, "/"))
            snprintf(cwdCopy, sizeof(cwdCopy), "/%s", local);
        else
            snprintf(cwdCopy, sizeof(cwdCopy), "%s/%s", gOpera.cwd, local);
        strcpy(local, cwdCopy);
        cursor = local;
        dir = gOpera.root;
        /* Re-resolve from root after constructing an absolute path. */
        while (*cursor == '/')
            ++cursor;
    }

    if (opera_is_root_token(local))
    {
        if (flagsOut)
            *flagsOut = OPERA_DIRENT_DIR;
        if (startOut)
            *startOut = gOpera.root.start;
        if (blocksOut)
            *blocksOut = gOpera.root.blocks;
        if (bytesOut)
            *bytesOut = gOpera.root.blocks * OPERA_BLOCK_SIZE;
        return 1;
    }

    while (opera_split_next(&cursor, component, sizeof(component)))
    {
        if (!strcmp(component, "."))
            continue;
        if (!strcmp(component, ".."))
        {
            return 0;
        }
        if (!opera_dir_find_entry(&gOpera, dir, component, &flags, &start, &blocks, &bytes, NULL))
            return 0;
        if (*cursor == '\0')
        {
            if (flagsOut)
                *flagsOut = flags;
            if (startOut)
                *startOut = start;
            if (blocksOut)
                *blocksOut = blocks;
            if (bytesOut)
                *bytesOut = bytes;
            return 1;
        }
        if ((flags & OPERA_DIRENT_TYPE_MASK) != OPERA_DIRENT_DIR)
            return 0;
        dir.start = start;
        dir.blocks = blocks;
        while (*cursor == '/')
            ++cursor;
    }
    return 0;
}

static int opera_make_absolute(const char *path, char *out, size_t cap)
{
    char normalized[OPERA_MAX_PATH];
    if (!path || !out || !cap)
        return 0;
    if (strlen(path) >= sizeof(normalized))
        return 0;
    strcpy(normalized, path);
    opera_normalize_separators(normalized);

    if (normalized[0] == '$' && !strncmp(normalized, "$progdir", 8))
    {
        const char *p = normalized + 8;
        if (*p == '/')
            ++p;
        snprintf(out, cap, "/%s", p);
        return 1;
    }
    if (normalized[0] == '^' && (normalized[1] == '/' || normalized[1] == '\0'))
    {
        /* ^/ is the 3DO root-relative spelling used by EFMM's LevelSequence. */
        const char *p = normalized + 1;
        if (*p == '/')
            ++p;
        snprintf(out, cap, "/%s", p);
        return 1;
    }
    if (normalized[0] == '/')
    {
        snprintf(out, cap, "%s", normalized);
        return 1;
    }
    if (!strcmp(gOpera.cwd, "/"))
        snprintf(out, cap, "/%s", normalized);
    else
        snprintf(out, cap, "%s/%s", gOpera.cwd, normalized);
    return strlen(out) < cap;
}

static void opera_rebuild_cwd_parent(const char *target)
{
    char temp[OPERA_MAX_PATH];
    size_t n;
    if (!target || !*target)
    {
        strcpy(gOpera.cwd, "/");
        return;
    }
    snprintf(temp, sizeof(temp), "%s", target);
    opera_normalize_separators(temp);
    while ((n = strlen(temp)) > 1 && temp[n - 1] == '/')
        temp[n - 1] = 0;
    while (n > 1)
    {
        if (temp[n - 1] == '/')
        {
            temp[n - 1] = 0;
            break;
        }
        --n;
    }
    snprintf(gOpera.cwd, sizeof(gOpera.cwd), "%s", n ? temp : "/");
}

int OperaFS_Mount(const char *imagePath)
{
    FILE *fp;
    OperaSectorFormat fmt;
    uint32_t blockCount;
    int64_t dataOffset;
    OperaDirRef root;

    OperaFS_Unmount();
    if (!imagePath || !*imagePath)
        return 0;
    fp = fopen(imagePath, "rb");
    if (!fp)
        return 0;
    if (!opera_detect_format(fp, &fmt, &blockCount, &root, &dataOffset))
    {
        fclose(fp);
        return 0;
    }

    gOpera.image = fp;
    gOpera.sectorFormat = fmt;
    gOpera.blockSize = OPERA_BLOCK_SIZE;
    gOpera.blockCount = blockCount;
    gOpera.dataOffset = dataOffset;
    gOpera.root = root;
    snprintf(gOpera.imagePath, sizeof(gOpera.imagePath), "%s", imagePath);
    strcpy(gOpera.cwd, "/");
    return 1;
}

void OperaFS_Unmount(void)
{
    if (gOpera.image)
        fclose(gOpera.image);
    memset(&gOpera, 0, sizeof(gOpera));
    strcpy(gOpera.cwd, "/");
}

int OperaFS_IsMounted(void)
{
    return gOpera.image != NULL;
}

const char *OperaFS_GetImagePath(void)
{
    return gOpera.imagePath;
}

OperaFile *OperaFS_Open(const char *path)
{
    OperaFile *file;
    uint32_t flags, start, blocks, bytes;
    if (!opera_lookup_path(path, &flags, &start, &blocks, &bytes))
        return NULL;
    if ((flags & OPERA_DIRENT_TYPE_MASK) == OPERA_DIRENT_DIR)
        return NULL;
    if (start >= gOpera.blockCount || blocks == 0 || bytes == 0)
        return NULL;
    if ((uint64_t)start + blocks > gOpera.blockCount)
        return NULL;

    file = (OperaFile *)calloc(1, sizeof(*file));
    if (!file)
        return NULL;
    file->startBlock = start;
    file->blockCount = blocks;
    file->byteCount = bytes;
    file->position = 0;
    return file;
}

size_t OperaFS_Read(OperaFile *file, void *buffer, size_t bytes)
{
    size_t total = 0;
    unsigned char block[OPERA_BLOCK_SIZE];
    if (!file || !buffer || !bytes || file->position >= (int64_t)file->byteCount)
        return 0;
    if ((uint64_t)bytes > (uint64_t)file->byteCount - (uint64_t)file->position)
        bytes = (size_t)((uint64_t)file->byteCount - (uint64_t)file->position);

    while (total < bytes)
    {
        int64_t pos = file->position;
        uint32_t blockInFile = (uint32_t)(pos / OPERA_BLOCK_SIZE);
        size_t inBlock = (size_t)(pos % OPERA_BLOCK_SIZE);
        size_t want = bytes - total;
        if (want > OPERA_BLOCK_SIZE - inBlock)
            want = OPERA_BLOCK_SIZE - inBlock;
        if (blockInFile >= file->blockCount)
            break;
        if (!opera_read_block_raw(&gOpera, file->startBlock + blockInFile, block))
            break;
        memcpy((unsigned char *)buffer + total, block + inBlock, want);
        file->position += (int64_t)want;
        total += want;
    }
    return total;
}

int64_t OperaFS_Seek(OperaFile *file, int64_t offset, int whence)
{
    int64_t target;
    if (!file)
        return -1;
    switch (whence)
    {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = file->position + offset;
        break;
    case SEEK_END:
        target = (int64_t)file->byteCount + offset;
        break;
    default:
        return -1;
    }
    if (target < 0)
        return -1;
    if (target > (int64_t)file->byteCount)
        target = (int64_t)file->byteCount;
    file->position = target;
    return target;
}

int64_t OperaFS_Tell(OperaFile *file)
{
    return file ? file->position : -1;
}

int64_t OperaFS_Size(OperaFile *file)
{
    return file ? (int64_t)file->byteCount : -1;
}

void OperaFS_Close(OperaFile *file)
{
    free(file);
}

int OperaFS_ChangeDirectory(const char *path)
{
    char absolute[OPERA_MAX_PATH];
    uint32_t flags, start, blocks, bytes;
    char normalized[OPERA_MAX_PATH];

    if (!OperaFS_IsMounted() || !path)
        return 0;
    if (opera_is_root_token(path))
    {
        strcpy(gOpera.cwd, "/");
        return 1;
    }
    if (strlen(path) >= sizeof(normalized))
        return 0;
    strcpy(normalized, path);
    opera_normalize_separators(normalized);

    if (!opera_make_absolute(normalized, absolute, sizeof(absolute)))
        return 0;

    if (!strcmp(normalized, "."))
        return 1;
    if (!strcmp(normalized, ".."))
    {
        opera_rebuild_cwd_parent(gOpera.cwd);
        return 1;
    }

    if (!opera_lookup_path(absolute, &flags, &start, &blocks, &bytes))
        return 0;
    if ((flags & OPERA_DIRENT_TYPE_MASK) != OPERA_DIRENT_DIR)
        return 0;

    snprintf(gOpera.cwd, sizeof(gOpera.cwd), "%s", absolute);
    if (gOpera.cwd[0] == 0)
        strcpy(gOpera.cwd, "/");
    return 1;
}

const char *OperaFS_GetCurrentDirectory(void)
{
    return gOpera.cwd;
}

int OperaFS_LoadFile(const char *path, uint8_t **data, size_t *size)
{
    OperaFile *file;
    uint8_t *buffer;
    int64_t fileSize;
    size_t got;

    if (data)
        *data = NULL;
    if (size)
        *size = 0;
    file = OperaFS_Open(path);
    if (!file)
        return 0;
    fileSize = OperaFS_Size(file);
    if (fileSize <= 0 || (uint64_t)fileSize > (uint64_t)SIZE_MAX)
    {
        OperaFS_Close(file);
        return 0;
    }
    buffer = (uint8_t *)malloc((size_t)fileSize);
    if (!buffer)
    {
        OperaFS_Close(file);
        return 0;
    }
    got = OperaFS_Read(file, buffer, (size_t)fileSize);
    OperaFS_Close(file);
    if (got != (size_t)fileSize)
    {
        free(buffer);
        return 0;
    }
    if (data)
        *data = buffer;
    if (size)
        *size = got;
    return 1;
}
