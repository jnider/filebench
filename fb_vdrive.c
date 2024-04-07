/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2024 Joel Nider
 */

#include "config.h"
#include "filebench.h"
#include "flowop.h"
#include "threadflow.h" /* For aiolist definition */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <strings.h>

#include "filebench.h"
#include "fsplug.h"

#include "fuse.h"
#include "posix.h"
#include "platform.h"

/*
	Connect to vdrive with integrated file system
*/

//static int fb_vdrive_freemem(fb_fdesc_t *fd, off64_t size);
static int fb_vdrive_open(fb_fdesc_t *, char *, int, int);
static int fb_vdrive_pread(fb_fdesc_t *, caddr_t, fbint_t, off64_t);
static int fb_vdrive_read(fb_fdesc_t *, caddr_t, fbint_t);
static int fb_vdrive_pwrite(fb_fdesc_t *, caddr_t, fbint_t, off64_t);
static int fb_vdrive_write(fb_fdesc_t *, caddr_t, fbint_t);
static int fb_vdrive_lseek(fb_fdesc_t *, off64_t, int);
static int fb_vdrive_truncate(fb_fdesc_t *, off64_t);
static int fb_vdrive_rename(const char *, const char *);
static int fb_vdrive_close(fb_fdesc_t *);
static int fb_vdrive_link(const char *, const char *);
static int fb_vdrive_symlink(const char *, const char *);
static int fb_vdrive_unlink(char *);
static ssize_t fb_vdrive_readlink(const char *, char *, size_t);
static int fb_vdrive_mkdir(char *, int);
static int fb_vdrive_rmdir(char *);
static DIR *fb_vdrive_opendir(char *);
static struct dirent *fb_vdrive_readdir(DIR *);
static int fb_vdrive_closedir(DIR *);
static int fb_vdrive_fsync(fb_fdesc_t *);
static int fb_vdrive_stat(char *, struct stat64 *);
static int fb_vdrive_fstat(fb_fdesc_t *, struct stat64 *);
static int fb_vdrive_access(const char *, int);
static void fb_vdrive_recur_rm(char *);

static fsplug_func_t fb_vdrive_funcs =
{
	"vdrive",
	NULL,		/* flush page cache */
	fb_vdrive_open,		/* open */
	fb_vdrive_pread,		/* pread */
	fb_vdrive_read,		/* read */
	fb_vdrive_pwrite,		/* pwrite */
	fb_vdrive_write,		/* write */
	fb_vdrive_lseek,		/* lseek */
	fb_vdrive_truncate,	/* ftruncate */
	fb_vdrive_rename,		/* rename */
	fb_vdrive_close,		/* close */
	fb_vdrive_link,		/* link */
	fb_vdrive_symlink,		/* symlink */
	fb_vdrive_unlink,		/* unlink */
	fb_vdrive_readlink,	/* readlink */
	fb_vdrive_mkdir,		/* mkdir */
	fb_vdrive_rmdir,		/* rmdir */
	fb_vdrive_opendir,		/* opendir */
	fb_vdrive_readdir,		/* readdir */
	fb_vdrive_closedir,	/* closedir */
	fb_vdrive_fsync,		/* fsync */
	fb_vdrive_stat,		/* stat */
	fb_vdrive_fstat,		/* fstat */
	fb_vdrive_access,		/* access */
	fb_vdrive_recur_rm		/* recursive rm */
};

/*
	Register this plugin
*/
void
fb_vdrive_funcvecinit(void)
{
	fs_functions_vec = &fb_vdrive_funcs;
}

/*
	Initialize the library
*/
int
fb_vdrive_init(void)
{
	int ret = platform_init(0, 1);
	if (ret < 0)
		printf("Error %i initializing virtio drive\n", ret);

	return ret;
}

/*
	Shut down the connection
	Abandon ship
	Close the 3-ring circus
*/
void fb_vdrive_shutdown(void)
{
	platform_cleanup();
}

static int fb_vdrive_open(fb_fdesc_t *fd, char *path, int flags, int perms)
{
	int ino;

	ino = g_open(path, flags);
	if (ino < 0)
		return FILEBENCH_ERROR;

	fd->fd_num = ino;

	return FILEBENCH_OK;
}

static int fb_vdrive_pread(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize, off64_t offset)
{
	return g_pread(fd->fd_num, offset, iosize, (uint8_t *)iobuf);
}

static int fb_vdrive_read(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize)
{
	return g_read(fd->fd_num, iosize, (uint8_t *)iobuf);
}

static int fb_vdrive_pwrite(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize, off64_t offset)
{
	return g_pwrite(fd->fd_num, iosize, offset, (uint8_t *)iobuf);
}

static int fb_vdrive_write(fb_fdesc_t *fd, caddr_t iobuf, fbint_t iosize)
{
	return g_write(fd->fd_num, iosize, (uint8_t *)iobuf);
}

static int fb_vdrive_lseek(fb_fdesc_t *fd, off64_t offset, int whence)
{
	return g_lseek(fd->fd_num, offset, whence);
}

static int fb_vdrive_truncate(fb_fdesc_t *fd, off64_t fse_size)
{
	//fuse_setattr(drv->fs, fd->fd_num, fa, FUSE_SET_ATTR_SIZE);
	printf("%s not implemented\n", __func__);
	return -1;
}

static int fb_vdrive_rename(const char *, const char *)
{
	printf("%s not implemented\n", __func__);
	return -1;
}

static int fb_vdrive_close(fb_fdesc_t *fd)
{
	return g_close(fd->fd_num);
}

static int fb_vdrive_link(const char *, const char *)
{
	printf("%s not implemented\n", __func__);
	return -1;
}

static int fb_vdrive_symlink(const char *, const char *)
{
	printf("%s not implemented\n", __func__);
	return -1;
}

static int fb_vdrive_unlink(char *path)
{
	return g_unlink(path);
}

static ssize_t fb_vdrive_readlink(const char *, char *, size_t)
{
	printf("%s not implemented\n", __func__);
	return -1;
}

static int fb_vdrive_mkdir(char *path, int perm)
{
	return g_mkdir(path, perm);
}

static int fb_vdrive_rmdir(char *)
{
	printf("%s not implemented\n", __func__);
	return -1;
}

static DIR *fb_vdrive_opendir(char *path)
{
	return g_opendir(path);
}

static struct dirent *fb_vdrive_readdir(DIR *dir)
{
	printf("%s not implemented\n", __func__);
	//g_readdir(dir->fd);
	return NULL;
}

static int fb_vdrive_closedir(DIR *dir)
{
	return 0;
}

static int fb_vdrive_fsync(fb_fdesc_t *fd)
{
	return g_fsync(fd->fd_num, 0, 0, 1);
}

static int fb_vdrive_stat(char *path, struct stat64 *statbuf)
{
	//printf("%s stat64=%lu stat=%lu\n", __func__, sizeof(struct stat64), sizeof(struct stat));
	return g_stat(path, (struct stat *)statbuf);
}

static int fb_vdrive_fstat(fb_fdesc_t *, struct stat64 *)
{
	printf("%s not implemented\n", __func__);
	return -1;
}

static int fb_vdrive_access(const char *, int)
{
	printf("%s not implemented\n", __func__);
	return -1;
}

static void fb_vdrive_recur_rm(char *path)
{
	printf("%s not implemented: path=%s\n", __func__, path);
}
