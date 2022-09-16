/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/blkpg.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#include "sd-device.h"

#include "alloc-util.h"
#include "blockdev-util.h"
#include "btrfs-util.h"
#include "device-util.h"
#include "devnum-util.h"
#include "dirent-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "missing_magic.h"
#include "parse-util.h"

int block_get_whole_disk(dev_t d, dev_t *ret) {
        char p[SYS_BLOCK_PATH_MAX("/partition")];
        _cleanup_free_ char *s = NULL;
        dev_t devt;
        int r;

        assert(ret);

        if (major(d) == 0)
                return -ENODEV;

        /* If it has a queue this is good enough for us */
        xsprintf_sys_block_path(p, "/queue", d);
        if (access(p, F_OK) >= 0) {
                *ret = d;
                return 0;
        }
        if (errno != ENOENT)
                return -errno;

        /* If it is a partition find the originating device */
        xsprintf_sys_block_path(p, "/partition", d);
        if (access(p, F_OK) < 0)
                return -errno;

        /* Get parent dev_t */
        xsprintf_sys_block_path(p, "/../dev", d);
        r = read_one_line_file(p, &s);
        if (r < 0)
                return r;

        r = parse_devnum(s, &devt);
        if (r < 0)
                return r;

        /* Only return this if it is really good enough for us. */
        xsprintf_sys_block_path(p, "/queue", devt);
        if (access(p, F_OK) < 0)
                return -errno;

        *ret = devt;
        return 1;
}

int get_block_device_fd(int fd, dev_t *ret) {
        struct stat st;
        int r;

        assert(fd >= 0);
        assert(ret);

        /* Gets the block device directly backing a file system. If the block device is encrypted, returns
         * the device mapper block device. */

        if (fstat(fd, &st))
                return -errno;

        if (major(st.st_dev) != 0) {
                *ret = st.st_dev;
                return 1;
        }

        r = btrfs_get_block_device_fd(fd, ret);
        if (r > 0)
                return 1;
        if (r != -ENOTTY) /* not btrfs */
                return r;

        *ret = 0;
        return 0;
}

int get_block_device(const char *path, dev_t *ret) {
        _cleanup_close_ int fd = -1;

        assert(path);
        assert(ret);

        fd = open(path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        return get_block_device_fd(fd, ret);
}

int block_get_originating(dev_t dt, dev_t *ret) {
        _cleanup_closedir_ DIR *d = NULL;
        _cleanup_free_ char *t = NULL;
        char p[SYS_BLOCK_PATH_MAX("/slaves")];
        _cleanup_free_ char *first_found = NULL;
        const char *q;
        dev_t devt;
        int r;

        /* For the specified block device tries to chase it through the layers, in case LUKS-style DM stacking is used,
         * trying to find the next underlying layer.  */

        xsprintf_sys_block_path(p, "/slaves", dt);
        d = opendir(p);
        if (!d)
                return -errno;

        FOREACH_DIRENT_ALL(de, d, return -errno) {

                if (dot_or_dot_dot(de->d_name))
                        continue;

                if (!IN_SET(de->d_type, DT_LNK, DT_UNKNOWN))
                        continue;

                if (first_found) {
                        _cleanup_free_ char *u = NULL, *v = NULL, *a = NULL, *b = NULL;

                        /* We found a device backed by multiple other devices. We don't really support
                         * automatic discovery on such setups, with the exception of dm-verity partitions. In
                         * this case there are two backing devices: the data partition and the hash
                         * partition. We are fine with such setups, however, only if both partitions are on
                         * the same physical device.  Hence, let's verify this by iterating over every node
                         * in the 'slaves/' directory and comparing them with the first that gets returned by
                         * readdir(), to ensure they all point to the same device. */

                        u = path_join(p, de->d_name, "../dev");
                        if (!u)
                                return -ENOMEM;

                        v = path_join(p, first_found, "../dev");
                        if (!v)
                                return -ENOMEM;

                        r = read_one_line_file(u, &a);
                        if (r < 0)
                                return log_debug_errno(r, "Failed to read %s: %m", u);

                        r = read_one_line_file(v, &b);
                        if (r < 0)
                                return log_debug_errno(r, "Failed to read %s: %m", v);

                        /* Check if the parent device is the same. If not, then the two backing devices are on
                         * different physical devices, and we don't support that. */
                        if (!streq(a, b))
                                return -ENOTUNIQ;
                } else {
                        first_found = strdup(de->d_name);
                        if (!first_found)
                                return -ENOMEM;
                }
        }

        if (!first_found)
                return -ENOENT;

        q = strjoina(p, "/", first_found, "/dev");

        r = read_one_line_file(q, &t);
        if (r < 0)
                return r;

        r = parse_devnum(t, &devt);
        if (r < 0)
                return -EINVAL;

        if (major(devt) == 0)
                return -ENOENT;

        *ret = devt;
        return 1;
}

int get_block_device_harder_fd(int fd, dev_t *ret) {
        int r;

        assert(fd >= 0);
        assert(ret);

        /* Gets the backing block device for a file system, and handles LUKS encrypted file systems, looking for its
         * immediate parent, if there is one. */

        r = get_block_device_fd(fd, ret);
        if (r <= 0)
                return r;

        r = block_get_originating(*ret, ret);
        if (r < 0)
                log_debug_errno(r, "Failed to chase block device, ignoring: %m");

        return 1;
}

int get_block_device_harder(const char *path, dev_t *ret) {
        _cleanup_close_ int fd = -1;

        assert(path);
        assert(ret);

        fd = open(path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        return get_block_device_harder_fd(fd, ret);
}

int lock_whole_block_device(dev_t devt, int operation) {
        _cleanup_close_ int lock_fd = -1;
        dev_t whole_devt;
        int r;

        /* Let's get a BSD file lock on the whole block device, as per: https://systemd.io/BLOCK_DEVICE_LOCKING */

        r = block_get_whole_disk(devt, &whole_devt);
        if (r < 0)
                return r;

        lock_fd = r = device_open_from_devnum(S_IFBLK, whole_devt, O_RDONLY|O_CLOEXEC|O_NONBLOCK, NULL);
        if (r < 0)
                return r;

        if (flock(lock_fd, operation) < 0)
                return -errno;

        return TAKE_FD(lock_fd);
}

int blockdev_partscan_enabled(int fd) {
        _cleanup_free_ char *p = NULL, *buf = NULL;
        unsigned long long ull;
        struct stat st;
        int r;

        /* Checks if partition scanning is correctly enabled on the block device */

        if (fstat(fd, &st) < 0)
                return -errno;

        if (!S_ISBLK(st.st_mode))
                return -ENOTBLK;

        if (asprintf(&p, "/sys/dev/block/%u:%u/capability", major(st.st_rdev), minor(st.st_rdev)) < 0)
                return -ENOMEM;

        r = read_one_line_file(p, &buf);
        if (r == -ENOENT) /* If the capability file doesn't exist then we are most likely looking at a
                           * partition block device, not the whole block device. And that means we have no
                           * partition scanning on for it (we do for its parent, but not for the partition
                           * itself). */
                return false;
        if (r < 0)
                return r;

        r = safe_atollu_full(buf, 16, &ull);
        if (r < 0)
                return r;

#ifndef GENHD_FL_NO_PART_SCAN
#define GENHD_FL_NO_PART_SCAN (0x0200)
#endif

        return !FLAGS_SET(ull, GENHD_FL_NO_PART_SCAN);
}

static int blockdev_is_encrypted(const char *sysfs_path, unsigned depth_left) {
        _cleanup_free_ char *p = NULL, *uuids = NULL;
        _cleanup_closedir_ DIR *d = NULL;
        int r, found_encrypted = false;

        assert(sysfs_path);

        if (depth_left == 0)
                return -EINVAL;

        p = path_join(sysfs_path, "dm/uuid");
        if (!p)
                return -ENOMEM;

        r = read_one_line_file(p, &uuids);
        if (r != -ENOENT) {
                if (r < 0)
                        return r;

                /* The DM device's uuid attribute is prefixed with "CRYPT-" if this is a dm-crypt device. */
                if (startswith(uuids, "CRYPT-"))
                        return true;
        }

        /* Not a dm-crypt device itself. But maybe it is on top of one? Follow the links in the "slaves/"
         * subdir. */

        p = mfree(p);
        p = path_join(sysfs_path, "slaves");
        if (!p)
                return -ENOMEM;

        d = opendir(p);
        if (!d) {
                if (errno == ENOENT) /* Doesn't have underlying devices */
                        return false;

                return -errno;
        }

        for (;;) {
                _cleanup_free_ char *q = NULL;
                struct dirent *de;

                errno = 0;
                de = readdir_no_dot(d);
                if (!de) {
                        if (errno != 0)
                                return -errno;

                        break; /* No more underlying devices */
                }

                q = path_join(p, de->d_name);
                if (!q)
                        return -ENOMEM;

                r = blockdev_is_encrypted(q, depth_left - 1);
                if (r < 0)
                        return r;
                if (r == 0) /* we found one that is not encrypted? then propagate that immediately */
                        return false;

                found_encrypted = true;
        }

        return found_encrypted;
}

int fd_is_encrypted(int fd) {
        char p[SYS_BLOCK_PATH_MAX(NULL)];
        dev_t devt;
        int r;

        r = get_block_device_fd(fd, &devt);
        if (r < 0)
                return r;
        if (r == 0) /* doesn't have a block device */
                return false;

        xsprintf_sys_block_path(p, NULL, devt);

        return blockdev_is_encrypted(p, 10 /* safety net: maximum recursion depth */);
}

int path_is_encrypted(const char *path) {
        char p[SYS_BLOCK_PATH_MAX(NULL)];
        dev_t devt;
        int r;

        r = get_block_device(path, &devt);
        if (r < 0)
                return r;
        if (r == 0) /* doesn't have a block device */
                return false;

        xsprintf_sys_block_path(p, NULL, devt);

        return blockdev_is_encrypted(p, 10 /* safety net: maximum recursion depth */);
}

int fd_get_whole_disk(int fd, bool backing, dev_t *ret) {
        dev_t devt;
        struct stat st;
        int r;

        assert(ret);

        if (fstat(fd, &st) < 0)
                return -errno;

        if (S_ISBLK(st.st_mode))
                devt = st.st_rdev;
        else if (!backing)
                return -ENOTBLK;
        else if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
                return -ENOTBLK;
        else if (major(st.st_dev) != 0)
                devt = st.st_dev;
        else {
                _cleanup_close_ int regfd = -1;

                /* If major(st.st_dev) is zero, this might mean we are backed by btrfs, which needs special
                 * handing, to get the backing device node. */

                regfd = fd_reopen(fd, O_RDONLY|O_CLOEXEC|O_NONBLOCK);
                if (regfd < 0)
                        return regfd;

                r = btrfs_get_block_device_fd(regfd, &devt);
                if (r == -ENOTTY)
                        return -ENOTBLK;
                if (r < 0)
                        return r;
        }

        return block_get_whole_disk(devt, ret);
}

int path_get_whole_disk(const char *path, bool backing, dev_t *ret) {
        _cleanup_close_ int fd = -1;

        fd = open(path, O_CLOEXEC|O_PATH);
        if (fd < 0)
                return -errno;

        return fd_get_whole_disk(fd, backing, ret);
}

int block_device_add_partition(
                int fd,
                const char *name,
                int nr,
                uint64_t start,
                uint64_t size) {

        assert(fd >= 0);
        assert(name);
        assert(nr > 0);

        struct blkpg_partition bp = {
                .pno = nr,
                .start = start,
                .length = size,
        };

        struct blkpg_ioctl_arg ba = {
                .op = BLKPG_ADD_PARTITION,
                .data = &bp,
                .datalen = sizeof(bp),
        };

        if (strlen(name) >= sizeof(bp.devname))
                return -EINVAL;

        strcpy(bp.devname, name);

        return RET_NERRNO(ioctl(fd, BLKPG, &ba));
}

int block_device_remove_partition(
                int fd,
                const char *name,
                int nr) {

        assert(fd >= 0);
        assert(name);
        assert(nr > 0);

        struct blkpg_partition bp = {
                .pno = nr,
        };

        struct blkpg_ioctl_arg ba = {
                .op = BLKPG_DEL_PARTITION,
                .data = &bp,
                .datalen = sizeof(bp),
        };

        if (strlen(name) >= sizeof(bp.devname))
                return -EINVAL;

        strcpy(bp.devname, name);

        return RET_NERRNO(ioctl(fd, BLKPG, &ba));
}

int block_device_resize_partition(
                int fd,
                int nr,
                uint64_t start,
                uint64_t size) {

        assert(fd >= 0);
        assert(nr > 0);

        struct blkpg_partition bp = {
                .pno = nr,
                .start = start,
                .length = size,
        };

        struct blkpg_ioctl_arg ba = {
                .op = BLKPG_RESIZE_PARTITION,
                .data = &bp,
                .datalen = sizeof(bp),
        };

        return RET_NERRNO(ioctl(fd, BLKPG, &ba));
}

int partition_enumerator_new(sd_device *dev, sd_device_enumerator **ret) {
        _cleanup_(sd_device_enumerator_unrefp) sd_device_enumerator *e = NULL;
        const char *s;
        int r;

        assert(dev);
        assert(ret);

        r = sd_device_get_subsystem(dev, &s);
        if (r < 0)
                return r;

        if (!streq(s, "block"))
                return -ENOTBLK;

        r = sd_device_get_devtype(dev, &s);
        if (r < 0)
                return r;

        if (!streq(s, "disk")) /* Refuse invocation on partition block device, insist on "whole" device */
                return -EINVAL;

        r = sd_device_enumerator_new(&e);
        if (r < 0)
                return r;

        r = sd_device_enumerator_allow_uninitialized(e);
        if (r < 0)
                return r;

        r = sd_device_enumerator_add_match_parent(e, dev);
        if (r < 0)
                return r;

        r = sd_device_get_sysname(dev, &s);
        if (r < 0)
                return r;

        /* Also add sysname check for safety. Hopefully, this also improves performance. */
        s = strjoina(s, "*");
        r = sd_device_enumerator_add_match_sysname(e, s);
        if (r < 0)
                return r;

        r = sd_device_enumerator_add_match_subsystem(e, "block", /* match = */ true);
        if (r < 0)
                return r;

        r = sd_device_enumerator_add_match_property(e, "DEVTYPE", "partition");
        if (r < 0)
                return r;

        *ret = TAKE_PTR(e);
        return 0;
}

int block_device_remove_all_partitions(sd_device *dev, int fd) {
        _cleanup_(sd_device_enumerator_unrefp) sd_device_enumerator *e = NULL;
        _cleanup_(sd_device_unrefp) sd_device *dev_unref = NULL;
        _cleanup_close_ int fd_close = -1;
        bool has_partitions = false;
        sd_device *part;
        int r, k = 0;

        assert(dev || fd >= 0);

        if (!dev) {
                struct stat st;

                if (fstat(fd, &st) < 0)
                        return -errno;

                r = sd_device_new_from_stat_rdev(&dev_unref, &st);
                if (r < 0)
                        return r;

                dev = dev_unref;
        }

        r = partition_enumerator_new(dev, &e);
        if (r < 0)
                return r;

        if (fd < 0) {
                fd_close = sd_device_open(dev, O_CLOEXEC|O_NONBLOCK|O_NOCTTY|O_RDONLY);
                if (fd_close < 0)
                        return fd_close;

                fd = fd_close;
        }

        FOREACH_DEVICE(e, part) {
                const char *v, *devname;
                int nr;

                has_partitions = true;

                r = sd_device_get_devname(part, &devname);
                if (r < 0)
                        return r;

                r = sd_device_get_property_value(part, "PARTN", &v);
                if (r < 0)
                        return r;

                r = safe_atoi(v, &nr);
                if (r < 0)
                        return r;

                r = block_device_remove_partition(fd, devname, nr);
                if (r == -ENODEV) {
                        log_debug("Kernel removed partition %s before us, ignoring", devname);
                        continue;
                }
                if (r < 0) {
                        log_debug_errno(r, "Failed to remove partition %s: %m", devname);
                        k = k < 0 ? k : r;
                        continue;
                }

                log_debug("Removed partition %s", devname);
        }

        return k < 0 ? k : has_partitions;
}

int block_device_has_partitions(sd_device *dev) {
        _cleanup_(sd_device_enumerator_unrefp) sd_device_enumerator *e = NULL;
        int r;

        assert(dev);

        /* Checks if the specified device currently has partitions. */

        r = partition_enumerator_new(dev, &e);
        if (r < 0)
                return r;

        return !!sd_device_enumerator_get_device_first(e);
}

int blockdev_reread_partition_table(sd_device *dev) {
        _cleanup_close_ int fd = -1;

        assert(dev);

        /* Try to re-read the partition table. This only succeeds if none of the devices is busy. */

        fd = sd_device_open(dev, O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);
        if (fd < 0)
                return fd;

        if (flock(fd, LOCK_EX|LOCK_NB) < 0)
                return -errno;

        if (ioctl(fd, BLKRRPART, 0) < 0)
                return -errno;

        return 0;
}
