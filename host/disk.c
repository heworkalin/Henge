#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* posix_fadvise */
#endif

#include <errno.h>
#include "../core/machine.h"
#include "disk.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

#define DISK_IO_CHUNK (128u << 20)

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly) {
    d->wfd = -1;
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (d->fd < 0) return -1;
    struct stat st;
    if (fstat(d->fd, &st) != 0 || (uint64_t)st.st_size < offset) {
        close(d->fd);
        d->fd = -1;
        return -1;
    }
    d->offset = offset;
    d->size = (uint64_t)st.st_size - offset;
    d->readonly = readonly;
    /* HV_WRITE_CACHE=0: writes skip the host's cache, so a long run of them cannot pile up
       gigabytes of dirty pages; reads stay cached, and the kernel keeps the two coherent. Off by default:
       on the Mac bench big writes took 2-3x as long that way (2 GB + sync 4-6 s -> 9-17 s) and everyday
       work was no faster. The phone's installer turns it on for the restore, which writes ~15 GB at once.

       macOS does this with F_NOCACHE on a second descriptor. Linux has no per-descriptor
       "do not cache" flag: O_DIRECT would be the analogue but it requires aligned buffers,
       which the guest's DMA does not give. What is available is to push each written range out
       and then drop it — see hv_disk_write() — so the dirty set stays bounded, which is all the
       F_NOCACHE path was for. */
    const char *wc = getenv("HV_WRITE_CACHE");
    if (!readonly && wc && !atoi(wc)) {
        d->wfd = open(path, O_RDWR);
        if (d->wfd < 0) d->wfd = -1;
#if defined(__APPLE__)
        if (d->wfd >= 0 && fcntl(d->wfd, F_NOCACHE, 1) == -1) {
            close(d->wfd);
            d->wfd = -1;
        }
#endif
    }
    return 0;
}

void hv_disk_close(HvDisk *d) {
    if (d->fd >= 0) close(d->fd);
    if (d->wfd >= 0) close(d->wfd);
    d->fd = d->wfd = -1;
}

int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        ssize_t n = pread(d->fd, p, count, (off_t)(d->offset + off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    if (d->readonly || off > d->size || len > d->size - off) return -1;
    const uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        off_t at = (off_t)(d->offset + off);
        ssize_t n = pwrite(d->wfd >= 0 ? d->wfd : d->fd, p, count, at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
#if !defined(__APPLE__)
        /* The Linux stand-in for F_NOCACHE: get the range written back, then drop it, so the
           dirty set does not grow without bound during the 15 GB restore.

           sync_file_range() would be the precise tool -- it starts writeback without
           waiting for it -- but bionic does not export it, and reaching it through
           syscall(__NR_sync_file_range) is more portability surface than this hint is
           worth. fdatasync() waits, which is slower but needs no per-architecture
           syscall table; the path is opt-in and already the slow one. A failure is
           harmless: the data is still correct, only still cached. */
        if (d->wfd >= 0) {
            fdatasync(d->wfd);
            posix_fadvise(d->wfd, at, (off_t)n, POSIX_FADV_DONTNEED);
        }
#endif
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

/* HV_FLUSH: 0 fsync; 1 nothing here, what was written is safe in the host's cache as long as the
   host runs; 2 a device barrier. macOS spells the barrier F_BARRIERFSYNC, which orders the writes
   without waiting for the platter; Linux's fdatasync is the same promise, and is what mkfs-level
   tools use for it. fsync would also work and would flush metadata needlessly. */
int hv_disk_flush(HvDisk *d) {
    if (d->readonly) return 0;
    switch (hv_knob[HV_K_FLUSH]) {
    case 1: return 0;
    case 2:
#if defined(__APPLE__)
        return fcntl(d->fd, F_BARRIERFSYNC) == -1 ? -1 : 0;
#else
        return fdatasync(d->fd);
#endif
    default: return fsync(d->fd);
    }
}
