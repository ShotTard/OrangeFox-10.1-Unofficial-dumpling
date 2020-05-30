/*
 * Copyright 2006 The Android Open Source Project
 *
 * System utilities.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "sysutil"
#include "Log.h"
#include "SysUtil.h"

static int getFileStartAndLength(int fd, loff_t *start_, size_t *length_)
{
    loff_t start, end;
    size_t length;

    assert(start_ != NULL);
    assert(length_ != NULL);

    // TODO: isn't start always 0 for the single call site? just use fstat instead?

    start = TEMP_FAILURE_RETRY(lseek64(fd, 0L, SEEK_CUR));
    end = TEMP_FAILURE_RETRY(lseek64(fd, 0L, SEEK_END));

    if (TEMP_FAILURE_RETRY(lseek64(fd, start, SEEK_SET)) == -1 ||
                start == (loff_t) -1 || end == (loff_t) -1) {
        LOGE("could not determine length of file\n");
        return -1;
    }

    length = end - start;
    if (length == 0) {
        LOGE("file is empty\n");
        return -1;
    }

    *start_ = start;
    *length_ = length;

    return 0;
}

/*
 * Map a file (from fd's current offset) into a private, read-only memory
 * segment.  The file offset must be a multiple of the page size.
 *
 * On success, returns 0 and fills out "pMap".  On failure, returns a nonzero
 * value and does not disturb "pMap".
 */
static bool sysMapFD(int fd, MemMapping* pMap) {
    loff_t start;
    size_t length;
    void* memPtr;

    assert(pMap != NULL);

    if (getFileStartAndLength(fd, &start, &length) < 0)
        return -1;

#if (PLATFORM_SDK_VERSION >= 21)
    memPtr = mmap64(NULL, length, PROT_READ, MAP_PRIVATE, fd, start);
#else
    // Older versions of Android do not have mmap64 so we will just use mmap instead
    memPtr = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, start);
#endif
    if (memPtr == MAP_FAILED) {
        LOGE("mmap(%d, R, PRIVATE, %d, 0) failed: %s\n", (int) length, fd, strerror(errno));
        return false;
    }

    pMap->addr = memPtr;
    pMap->length = length;
    pMap->range_count = 1;
    pMap->ranges = malloc(sizeof(MappedRange));
    if (pMap->ranges == NULL) {
        LOGE("malloc failed: %s\n", strerror(errno));
        munmap(memPtr, length);
        return false;
    }
    pMap->ranges[0].addr = memPtr;
    pMap->ranges[0].length = length;

    return true;
}

static int sysMapBlockFile(FILE* mapf, MemMapping* pMap)
{
    char block_dev[PATH_MAX+1];
    size_t size;
    unsigned int blksize;
    size_t blocks;
    unsigned int range_count;
    unsigned int i;

    if (fgets(block_dev, sizeof(block_dev), mapf) == NULL) {
        LOGE("failed to read block device from header\n");
        return -1;
    }
    for (i = 0; i < sizeof(block_dev); ++i) {
        if (block_dev[i] == '\n') {
            block_dev[i] = 0;
            break;
        }
    }

    if (fscanf(mapf, "%zu %u\n%u\n", &size, &blksize, &range_count) != 3) {
        LOGE("failed to parse block map header\n");
        return -1;
    }
    if (blksize != 0) {
        blocks = ((size-1) / blksize) + 1;
    }
    if (size == 0 || blksize == 0 || blocks > SIZE_MAX / blksize || range_count == 0) {
        LOGE("invalid data in block map file: size %zu, blksize %u, range_count %u\n",
             size, blksize, range_count);
        return -1;
    }

    pMap->range_count = range_count;
    pMap->ranges = calloc(range_count, sizeof(MappedRange));
    if (pMap->ranges == NULL) {
        LOGE("calloc(%u, %zu) failed: %s\n", range_count, sizeof(MappedRange), strerror(errno));
        return -1;
    }

    // Reserve enough contiguous address space for the whole file.
    unsigned char* reserve;
#if (PLATFORM_SDK_VERSION >= 21)
    reserve = mmap64(NULL, blocks * blksize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
#else
    // Older versions of Android do not have mmap64 so we will just use mmap instead
    reserve = mmap(NULL, blocks * blksize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (reserve == MAP_FAILED) {
        LOGE("failed to reserve address space: %s\n", strerror(errno));
        free(pMap->ranges);
        return -1;
    }

    int fd = open(block_dev, O_RDONLY);
    if (fd < 0) {
        LOGE("failed to open block device %s: %s\n", block_dev, strerror(errno));
        munmap(reserve, blocks * blksize);
        free(pMap->ranges);
        return -1;
    }

    unsigned char* next = reserve;
    size_t remaining_size = blocks * blksize;
    bool success = true;
    for (i = 0; i < range_count; ++i) {
        size_t start, end;
        if (fscanf(mapf, "%zu %zu\n", &start, &end) != 2) {
            LOGE("failed to parse range %d in block map\n", i);
            success = false;
            break;
        }
        size_t length = (end - start) * blksize;
        if (end <= start || (end - start) > SIZE_MAX / blksize || length > remaining_size) {
          LOGE("unexpected range in block map: %zu %zu\n", start, end);
          success = false;
          break;
        }
#if (PLATFORM_SDK_VERSION >= 21)
        void* addr = mmap64(next, (end-start)*blksize, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, ((off64_t)start)*blksize);
#else
        // Older versions of Android do not have mmap64 so we will just use mmap instead
        void* addr = mmap(next, (end-start)*blksize, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, ((off64_t)start)*blksize);
#endif
        if (addr == MAP_FAILED) {
            LOGE("failed to map block %d: %s\n", i, strerror(errno));
            success = false;
            break;
        }
        pMap->ranges[i].addr = addr;
        pMap->ranges[i].length = length;

        next += length;
        remaining_size -= length;
    }
    if (success && remaining_size != 0) {
      LOGE("ranges in block map are invalid: remaining_size = %zu\n", remaining_size);
      success = false;
    }
    if (!success) {
      close(fd);
      munmap(reserve, blocks * blksize);
      free(pMap->ranges);
      return -1;
    }

    close(fd);
    pMap->addr = reserve;
    pMap->length = size;

    LOGI("mmapped %d ranges\n", range_count);

    return 0;
}

int sysMapFile(const char* fn, MemMapping* pMap)
{
    memset(pMap, 0, sizeof(*pMap));

    if (fn && fn[0] == '@') {
        // A map of blocks
        FILE* mapf = fopen(fn+1, "r");
        if (mapf == NULL) {
            LOGE("Unable to open '%s': %s\n", fn+1, strerror(errno));
            return -1;
        }

        if (sysMapBlockFile(mapf, pMap) != 0) {
            LOGE("Map of '%s' failed\n", fn);
            fclose(mapf);
            return -1;
        }

        fclose(mapf);
    } else {
        // This is a regular file.
        int fd = open(fn, O_RDONLY);
        if (fd == -1) {
            LOGE("Unable to open '%s': %s\n", fn, strerror(errno));
            return -1;
        }

        if (!sysMapFD(fd, pMap)) {
            LOGE("Map of '%s' failed\n", fn);
            close(fd);
            return -1;
        }

        close(fd);
    }
    return 0;
}

/*
 * Release a memory mapping.
 */
void sysReleaseMap(MemMapping* pMap)
{
    int i;
    for (i = 0; i < pMap->range_count; ++i) {
        if (munmap(pMap->ranges[i].addr, pMap->ranges[i].length) < 0) {
            LOGE("munmap(%p, %d) failed: %s\n",
                 pMap->ranges[i].addr, (int)pMap->ranges[i].length, strerror(errno));
        }
    }
    free(pMap->ranges);
    pMap->ranges = NULL;
    pMap->range_count = 0;
}
