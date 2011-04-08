/*-
 * Copyright (c) 2003-2009 Tim Kientzle
 * Copyright (c) 2010,2011 Michihiro NAKAJIMA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "archive_platform.h"
__FBSDID("$FreeBSD$");

#if defined(_WIN32) && !defined(__CYGWIN__)

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif
#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif
#ifdef HAVE_LINUX_MAGIC_H
#include <linux/magic.h>
#endif
#ifdef HAVE_DIRECT_H
#include <direct.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined(HAVE_WINIOCTL_H) && !defined(__CYGWIN__)
#include <winioctl.h>
#endif

#include "archive.h"
#include "archive_string.h"
#include "archive_entry.h"
#include "archive_private.h"
#include "archive_read_disk_private.h"

#ifndef O_BINARY
#define O_BINARY	0
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
/* Old SDKs do not provide IO_REPARSE_TAG_SYMLINK */
#define	IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif

/*-
 * This is a new directory-walking system that addresses a number
 * of problems I've had with fts(3).  In particular, it has no
 * pathname-length limits (other than the size of 'int'), handles
 * deep logical traversals, uses considerably less memory, and has
 * an opaque interface (easier to modify in the future).
 *
 * Internally, it keeps a single list of "tree_entry" items that
 * represent filesystem objects that require further attention.
 * Non-directories are not kept in memory: they are pulled from
 * readdir(), returned to the client, then freed as soon as possible.
 * Any directory entry to be traversed gets pushed onto the stack.
 *
 * There is surprisingly little information that needs to be kept for
 * each item on the stack.  Just the name, depth (represented here as the
 * string length of the parent directory's pathname), and some markers
 * indicating how to get back to the parent (via chdir("..") for a
 * regular dir or via fchdir(2) for a symlink).
 */
/*
 * TODO:
 *    1) Loop checking.
 *    3) Arbitrary logical traversals by closing/reopening intermediate fds.
 */

struct restore_time {
	const wchar_t		*full_path;
	time_t			 mtime;
	long			 mtime_nsec;
	time_t			 atime;
	long			 atime_nsec;
	mode_t			 filetype;
};

struct tree_entry {
	int			 depth;
	struct tree_entry	*next;
	struct tree_entry	*parent;
	size_t			 full_path_dir_length;
	struct archive_wstring	 name;
	struct archive_wstring	 full_path;
	size_t			 dirname_length;
	int64_t			 dev;
	int64_t			 ino;
	int			 flags;
	int			 filesystem_id;
	/* How to restore time of a directory. */
	struct restore_time	 restore_time;
};

struct filesystem {
	int64_t		dev;
	int		synthetic;
	int		remote;
};

/* Definitions for tree_entry.flags bitmap. */
#define	isDir		1  /* This entry is a regular directory. */
#define	isDirLink	2  /* This entry is a symbolic link to a directory. */
#define	needsFirstVisit	4  /* This is an initial entry. */
#define	needsDescent	8  /* This entry needs to be previsited. */
#define	needsOpen	16 /* This is a directory that needs to be opened. */
#define	needsAscent	32 /* This entry needs to be postvisited. */

/*
 * On Windows, "first visit" is handled as a pattern to be handed to
 * _findfirst().  This is consistent with Windows conventions that
 * file patterns are handled within the application.  On Posix,
 * "first visit" is just returned to the client.
 */

/*
 * Local data for this package.
 */
struct tree {
	struct tree_entry	*stack;
	struct tree_entry	*current;
	HANDLE d;
#define	INVALID_DIR_HANDLE INVALID_HANDLE_VALUE
	WIN32_FIND_DATAW	_findData;
	WIN32_FIND_DATAW	*findData;
	int			 flags;
	int			 visit_type;
	/* Error code from last failed operation. */
	int			 tree_errno;

	/* A full path with "\\?\" prefix. */
	struct archive_wstring	 full_path;
	size_t			 full_path_dir_length;
	/* Dynamically-sized buffer for holding path */
	struct archive_wstring	 path;
	struct archive_string	 temp;

	/* Last path element */
	const wchar_t		*basename;
	/* Leading dir length */
	size_t			 dirname_length;

	int	 depth;

	BY_HANDLE_FILE_INFORMATION	lst;
	BY_HANDLE_FILE_INFORMATION	st;
	int			 descend;
	/* How to restore time of a file. */
	struct restore_time	restore_time;

	struct entry_sparse {
		int64_t		 length;
		int64_t		 offset;
	}			*sparse_list, *current_sparse;
	int			 sparse_count;
	int			 sparse_list_size;

	char			 initial_symlink_mode;
	char			 symlink_mode;
	struct filesystem	*current_filesystem;
	struct filesystem	*filesystem_table;
	int			 current_filesystem_id;
	int			 max_filesystem_id;
	int			 allocated_filesytem;
};

#define bhfi_dev(bhfi)	((bhfi)->dwVolumeSerialNumber)
/* Treat FileIndex as i-node. We should remove a sequence number
 * which is high-16-bits of nFileIndexHigh. */
#define bhfi_ino(bhfi)	\
	((((int64_t)((bhfi)->nFileIndexHigh & 0x0000FFFFUL)) << 32) \
    + (bhfi)->nFileIndexLow)

/* Definitions for tree.flags bitmap. */
#define	hasStat		16 /* The st entry is valid. */
#define	hasLstat	32 /* The lst entry is valid. */
#define	needsRestoreTimes 128

static int
tree_dir_next_windows(struct tree *t, const wchar_t *pattern);

#ifdef HAVE_DIRENT_D_NAMLEN
/* BSD extension; avoids need for a strlen() call. */
#define	D_NAMELEN(dp)	(dp)->d_namlen
#else
#define	D_NAMELEN(dp)	(strlen((dp)->d_name))
#endif

/* Initiate/terminate a tree traversal. */
static struct tree *tree_open(const char *, int, int);
static struct tree *tree_reopen(struct tree *, const char *, int);
static void tree_close(struct tree *);
static void tree_free(struct tree *);
static void tree_push(struct tree *, const wchar_t *, const wchar_t *,
		int, int64_t, int64_t, struct restore_time *);

/*
 * tree_next() returns Zero if there is no next entry, non-zero if
 * there is.  Note that directories are visited three times.
 * Directories are always visited first as part of enumerating their
 * parent; that is a "regular" visit.  If tree_descend() is invoked at
 * that time, the directory is added to a work list and will
 * subsequently be visited two more times: once just after descending
 * into the directory ("postdescent") and again just after ascending
 * back to the parent ("postascent").
 *
 * TREE_ERROR_DIR is returned if the descent failed (because the
 * directory couldn't be opened, for instance).  This is returned
 * instead of TREE_POSTDESCENT/TREE_POSTASCENT.  TREE_ERROR_DIR is not a
 * fatal error, but it does imply that the relevant subtree won't be
 * visited.  TREE_ERROR_FATAL is returned for an error that left the
 * traversal completely hosed.  Right now, this is only returned for
 * chdir() failures during ascent.
 */
#define	TREE_REGULAR		1
#define	TREE_POSTDESCENT	2
#define	TREE_POSTASCENT		3
#define	TREE_ERROR_DIR		-1
#define	TREE_ERROR_FATAL	-2

static int tree_next(struct tree *);

/*
 * Return information about the current entry.
 */

/*
 * The current full pathname, length of the full pathname, and a name
 * that can be used to access the file.  Because tree does use chdir
 * extensively, the access path is almost never the same as the full
 * current path.
 *
 */
static const wchar_t *tree_current_path(struct tree *);
static const wchar_t *tree_current_access_path(struct tree *);

/*
 * Request the lstat() or stat() data for the current path.  Since the
 * tree package needs to do some of this anyway, and caches the
 * results, you should take advantage of it here if you need it rather
 * than make a redundant stat() or lstat() call of your own.
 */
static const BY_HANDLE_FILE_INFORMATION *tree_current_stat(struct tree *);
static const BY_HANDLE_FILE_INFORMATION *tree_current_lstat(struct tree *);

/* The following functions use tricks to avoid a certain number of
 * stat()/lstat() calls. */
/* "is_physical_dir" is equivalent to S_ISDIR(tree_current_lstat()->st_mode) */
static int tree_current_is_physical_dir(struct tree *);
/* "is_physical_link" is equivalent to S_ISLNK(tree_current_lstat()->st_mode) */
static int tree_current_is_physical_link(struct tree *);
/* Instead of archive_entry_copy_stat for BY_HANDLE_FILE_INFORMATION */
static void tree_archive_entry_copy_bhfi(struct archive_entry *,
		    struct tree *, const BY_HANDLE_FILE_INFORMATION *);
/* "is_dir" is equivalent to S_ISDIR(tree_current_stat()->st_mode) */
static int tree_current_is_dir(struct tree *);
static int update_current_filesystem(struct archive_read_disk *a,
		    int64_t dev);
static int setup_current_filesystem(struct archive_read_disk *);
static int tree_target_is_same_as_parent(struct tree *,
		    const BY_HANDLE_FILE_INFORMATION *);

static int	_archive_read_free(struct archive *);
static int	_archive_read_close(struct archive *);
static int	_archive_read_data_block(struct archive *,
		    const void **, size_t *, int64_t *);
static int	_archive_read_next_header2(struct archive *,
		    struct archive_entry *);
#if ARCHIVE_VERSION_NUMBER < 3000000
static const char *trivial_lookup_gname(void *, gid_t gid);
static const char *trivial_lookup_uname(void *, uid_t uid);
#else
static const char *trivial_lookup_gname(void *, int64_t gid);
static const char *trivial_lookup_uname(void *, int64_t uid);
#endif
static int	setup_sparse(struct archive_read_disk *, struct archive_entry *);
static int	close_and_restore_time(int fd, struct tree *,
		    struct restore_time *);
static int	setup_sparse_from_disk(struct archive_read_disk *,
		    struct archive_entry *, HANDLE);



static struct archive_vtable *
archive_read_disk_vtable(void)
{
	static struct archive_vtable av;
	static int inited = 0;

	if (!inited) {
		av.archive_free = _archive_read_free;
		av.archive_close = _archive_read_close;
		av.archive_read_data_block = _archive_read_data_block;
		av.archive_read_next_header2 = _archive_read_next_header2;
		inited = 1;
	}
	return (&av);
}

#if ARCHIVE_VERSION_NUMBER < 3000000
const char *
archive_read_disk_gname(struct archive *_a, gid_t gid)
#else
const char *
archive_read_disk_gname(struct archive *_a, int64_t gid)
#endif
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	if (ARCHIVE_OK != __archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
		ARCHIVE_STATE_ANY, "archive_read_disk_gname"))
		return (NULL);
	if (a->lookup_gname == NULL)
		return (NULL);
	return ((*a->lookup_gname)(a->lookup_gname_data, gid));
}

#if ARCHIVE_VERSION_NUMBER < 3000000
const char *
archive_read_disk_uname(struct archive *_a, uid_t uid)
#else
const char *
archive_read_disk_uname(struct archive *_a, int64_t uid)
#endif
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	if (ARCHIVE_OK != __archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
		ARCHIVE_STATE_ANY, "archive_read_disk_uname"))
		return (NULL);
	if (a->lookup_uname == NULL)
		return (NULL);
	return ((*a->lookup_uname)(a->lookup_uname_data, uid));
}

#if ARCHIVE_VERSION_NUMBER < 3000000
int
archive_read_disk_set_gname_lookup(struct archive *_a,
    void *private_data,
    const char * (*lookup_gname)(void *private, gid_t gid),
    void (*cleanup_gname)(void *private))
#else
int
archive_read_disk_set_gname_lookup(struct archive *_a,
    void *private_data,
    const char * (*lookup_gname)(void *private, int64_t gid),
    void (*cleanup_gname)(void *private))
#endif
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	archive_check_magic(&a->archive, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_read_disk_set_gname_lookup");

	if (a->cleanup_gname != NULL && a->lookup_gname_data != NULL)
		(a->cleanup_gname)(a->lookup_gname_data);

	a->lookup_gname = lookup_gname;
	a->cleanup_gname = cleanup_gname;
	a->lookup_gname_data = private_data;
	return (ARCHIVE_OK);
}

#if ARCHIVE_VERSION_NUMBER < 3000000
int
archive_read_disk_set_uname_lookup(struct archive *_a,
    void *private_data,
    const char * (*lookup_uname)(void *private, uid_t uid),
    void (*cleanup_uname)(void *private))
#else
int
archive_read_disk_set_uname_lookup(struct archive *_a,
    void *private_data,
    const char * (*lookup_uname)(void *private, int64_t uid),
    void (*cleanup_uname)(void *private))
#endif
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	archive_check_magic(&a->archive, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_read_disk_set_uname_lookup");

	if (a->cleanup_uname != NULL && a->lookup_uname_data != NULL)
		(a->cleanup_uname)(a->lookup_uname_data);

	a->lookup_uname = lookup_uname;
	a->cleanup_uname = cleanup_uname;
	a->lookup_uname_data = private_data;
	return (ARCHIVE_OK);
}

/*
 * Create a new archive_read_disk object and initialize it with global state.
 */
struct archive *
archive_read_disk_new(void)
{
	struct archive_read_disk *a;

	a = (struct archive_read_disk *)malloc(sizeof(*a));
	if (a == NULL)
		return (NULL);
	memset(a, 0, sizeof(*a));
	a->archive.magic = ARCHIVE_READ_DISK_MAGIC;
	a->archive.state = ARCHIVE_STATE_NEW;
	a->archive.vtable = archive_read_disk_vtable();
	a->lookup_uname = trivial_lookup_uname;
	a->lookup_gname = trivial_lookup_gname;
	a->entry_wd_fd = -1;
	a->entry_fd = -1;
	return (&a->archive);
}

static int
_archive_read_free(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	int r;

	if (_a == NULL)
		return (ARCHIVE_OK);
	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY | ARCHIVE_STATE_FATAL, "archive_read_free");

	if (a->archive.state != ARCHIVE_STATE_CLOSED)
		r = _archive_read_close(&a->archive);
	else
		r = ARCHIVE_OK;

	tree_free(a->tree);
	if (a->cleanup_gname != NULL && a->lookup_gname_data != NULL)
		(a->cleanup_gname)(a->lookup_gname_data);
	if (a->cleanup_uname != NULL && a->lookup_uname_data != NULL)
		(a->cleanup_uname)(a->lookup_uname_data);
	archive_string_free(&a->archive.error_string);
	free(a->entry_buff);
	a->archive.magic = 0;
	free(a);
	return (r);
}

static int
_archive_read_close(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY | ARCHIVE_STATE_FATAL, "archive_read_close");

	if (a->archive.state != ARCHIVE_STATE_FATAL)
		a->archive.state = ARCHIVE_STATE_CLOSED;

	if (a->entry_fd >= 0) {
		if (a->tree != NULL && a->tree->flags & needsRestoreTimes)
			close_and_restore_time(a->entry_fd, a->tree,
			    &a->tree->restore_time);
		else
			close(a->entry_fd);
		a->entry_fd = -1;
	}
	if (a->tree != NULL)
		tree_close(a->tree);

	return (ARCHIVE_OK);
}

static void
setup_symlink_mode(struct archive_read_disk *a, char symlink_mode, 
    int follow_symlinks)
{
	a->symlink_mode = symlink_mode;
	a->follow_symlinks = follow_symlinks;
	if (a->tree != NULL) {
		a->tree->initial_symlink_mode = a->symlink_mode;
		a->tree->symlink_mode = a->symlink_mode;
	}
}

int
archive_read_disk_set_symlink_logical(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_read_disk_set_symlink_logical");
	setup_symlink_mode(a, 'L', 1);
	return (ARCHIVE_OK);
}

int
archive_read_disk_set_symlink_physical(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_read_disk_set_symlink_physical");
	setup_symlink_mode(a, 'P', 0);
	return (ARCHIVE_OK);
}

int
archive_read_disk_set_symlink_hybrid(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_read_disk_set_symlink_hybrid");
	setup_symlink_mode(a, 'H', 1);/* Follow symlinks initially. */
	return (ARCHIVE_OK);
}

int
archive_read_disk_set_atime_restored(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_ANY, "archive_read_disk_restore_atime");
	a->restore_time = 1;
	if (a->tree != NULL)
		a->tree->flags |= needsRestoreTimes;
	return (ARCHIVE_OK);
}

/*
 * Trivial implementations of gname/uname lookup functions.
 * These are normally overridden by the client, but these stub
 * versions ensure that we always have something that works.
 */
#if ARCHIVE_VERSION_NUMBER < 3000000
static const char *
trivial_lookup_gname(void *private_data, gid_t gid)
#else
static const char *
trivial_lookup_gname(void *private_data, int64_t gid)
#endif
{
	(void)private_data; /* UNUSED */
	(void)gid; /* UNUSED */
	return (NULL);
}

#if ARCHIVE_VERSION_NUMBER < 3000000
static const char *
trivial_lookup_uname(void *private_data, uid_t uid)
#else
static const char *
trivial_lookup_uname(void *private_data, int64_t uid)
#endif
{
	(void)private_data; /* UNUSED */
	(void)uid; /* UNUSED */
	return (NULL);
}

static int
_archive_read_data_block(struct archive *_a, const void **buff,
    size_t *size, int64_t *offset)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	struct tree *t = a->tree;
	int r;
	ssize_t bytes;
	size_t buffbytes;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC, ARCHIVE_STATE_DATA,
	    "archive_read_data_block");

	if (a->entry_eof || a->entry_remaining_bytes <= 0) {
		r = ARCHIVE_EOF;
		goto abort_read_data;
	}

	/* Allocate read buffer. */
	if (a->entry_buff == NULL) {
		a->entry_buff = malloc(1024 * 64);
		if (a->entry_buff == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Couldn't allocate memory");
			r = ARCHIVE_FATAL;
			a->archive.state = ARCHIVE_STATE_FATAL;
			goto abort_read_data;
		}
		a->entry_buff_size = 1024 * 64;
	}

	buffbytes = a->entry_buff_size;
	if (buffbytes > t->current_sparse->length)
		buffbytes = t->current_sparse->length;

	/*
	 * Skip hole.
	 */
	if (t->current_sparse->offset > a->entry_total) {
		if (lseek(a->entry_fd,
		    t->current_sparse->offset, SEEK_SET) < 0) {
			archive_set_error(&a->archive, errno, "Seek error");
			r = ARCHIVE_FATAL;
			a->archive.state = ARCHIVE_STATE_FATAL;
			goto abort_read_data;
		}
		bytes = t->current_sparse->offset - a->entry_total;
		a->entry_remaining_bytes -= bytes;
		a->entry_total += bytes;
	}
	if (buffbytes > 0) {
		bytes = read(a->entry_fd, a->entry_buff, buffbytes);
		if (bytes < 0) {
			archive_set_error(&a->archive, errno, "Read error");
			r = ARCHIVE_FATAL;
			a->archive.state = ARCHIVE_STATE_FATAL;
			goto abort_read_data;
		}
	} else
		bytes = 0;
	if (bytes == 0) {
		/* Get EOF */
		a->entry_eof = 1;
		r = ARCHIVE_EOF;
		goto abort_read_data;
	}
	*buff = a->entry_buff;
	*size = bytes;
	*offset = a->entry_total;
	a->entry_total += bytes;
	a->entry_remaining_bytes -= bytes;
	if (a->entry_remaining_bytes == 0) {
		/* Close the current file descriptor */
		close_and_restore_time(a->entry_fd, t, &t->restore_time);
		a->entry_fd = -1;
		a->entry_eof = 1;
	}
	t->current_sparse->offset += bytes;
	t->current_sparse->length -= bytes;
	if (t->current_sparse->length == 0 && !a->entry_eof)
		t->current_sparse++;
	return (ARCHIVE_OK);

abort_read_data:
	*buff = NULL;
	*size = 0;
	*offset = a->entry_total;
	if (a->entry_fd >= 0) {
		/* Close the current file descriptor */
		close_and_restore_time(a->entry_fd, t, &t->restore_time);
		a->entry_fd = -1;
	}
	return (r);
}

static int
_archive_read_next_header2(struct archive *_a, struct archive_entry *entry)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	struct tree *t;
	const BY_HANDLE_FILE_INFORMATION *st;
	const BY_HANDLE_FILE_INFORMATION *lst;
	const wchar_t *wp;
	const char*name;
	int descend, r;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
	    "archive_read_next_header2");

	if (a->entry_fd >= 0) {
		close(a->entry_fd);
		a->entry_fd = -1;
	}
	t = a->tree;
	st = NULL;
	lst = NULL;
	do {
		switch (tree_next(t)) {
		case TREE_ERROR_FATAL:
			archive_set_error(&a->archive, t->tree_errno,
			    "%ls: Unable to continue traversing directory tree",
			    tree_current_path(t));
			a->archive.state = ARCHIVE_STATE_FATAL;
			return (ARCHIVE_FATAL);
		case TREE_ERROR_DIR:
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "%ls: Couldn't visit directory",
			    tree_current_path(t));
			return (ARCHIVE_FAILED);
		case 0:
			return (ARCHIVE_EOF);
		case TREE_POSTDESCENT:
		case TREE_POSTASCENT:
			break;
		case TREE_REGULAR:
			lst = tree_current_lstat(t);
			if (lst == NULL) {
				archive_set_error(&a->archive, errno,
				    "%ls: Cannot stat",
				    tree_current_path(t));
				return (ARCHIVE_FAILED);
			}
			break;
		}	
	} while (lst == NULL);

	/*
	 * Distinguish 'L'/'P'/'H' symlink following.
	 */
	switch(t->symlink_mode) {
	case 'H':
		/* 'H': After the first item, rest like 'P'. */
		t->symlink_mode = 'P';
		/* 'H': First item (from command line) like 'L'. */
		/* FALLTHROUGH */
	case 'L':
		/* 'L': Do descend through a symlink to dir. */
		descend = tree_current_is_dir(t);
		/* 'L': Follow symlinks to files. */
		a->symlink_mode = 'L';
		a->follow_symlinks = 1;
		/* 'L': Archive symlinks as targets, if we can. */
		st = tree_current_stat(t);
		if (st != NULL && !tree_target_is_same_as_parent(t, st))
			break;
		/* If stat fails, we have a broken symlink;
		 * in that case, don't follow the link. */
		/* FALLTHROUGH */
	default:
		/* 'P': Don't descend through a symlink to dir. */
		descend = tree_current_is_physical_dir(t);
		/* 'P': Don't follow symlinks to files. */
		a->symlink_mode = 'P';
		a->follow_symlinks = 0;
		/* 'P': Archive symlinks as symlinks. */
		st = lst;
		break;
	}

	if (update_current_filesystem(a, bhfi_dev(st)) != ARCHIVE_OK) {
		a->archive.state = ARCHIVE_STATE_FATAL;
		return (ARCHIVE_FATAL);
	}
	t->descend = descend;

	archive_entry_copy_pathname_w(entry, tree_current_path(t));
	// TODO: create and use archive_entry_copy_sourcepath_w
	wp = tree_current_access_path(t);
	archive_string_empty(&(t->temp));
	archive_string_append_from_wcs_to_mbs(&(a->archive),
	    &(t->temp), wp, wcslen(wp));
	archive_entry_copy_sourcepath(entry, t->temp.s);
	tree_archive_entry_copy_bhfi(entry, t, st);

	/* Save the times to be restored. */
	t->restore_time.mtime = archive_entry_mtime(entry);
	t->restore_time.mtime_nsec = archive_entry_mtime_nsec(entry);
	t->restore_time.atime = archive_entry_atime(entry);
	t->restore_time.atime_nsec = archive_entry_atime_nsec(entry);
	t->restore_time.filetype = archive_entry_filetype(entry);

	/* Lookup uname/gname */
	name = archive_read_disk_uname(_a, archive_entry_uid(entry));
	if (name != NULL)
		archive_entry_copy_uname(entry, name);
	name = archive_read_disk_gname(_a, archive_entry_gid(entry));
	if (name != NULL)
		archive_entry_copy_gname(entry, name);

	r = ARCHIVE_OK;
	if (archive_entry_filetype(entry) == AE_IFREG &&
	    archive_entry_size(entry) > 0) {
		a->entry_fd = _wopen(tree_current_access_path(t),
		    O_RDONLY | O_BINARY);
		if (a->entry_fd < 0) {
			archive_set_error(&a->archive, errno,
			    "Couldn't open %ls", tree_current_path(a->tree));
			return (ARCHIVE_FAILED);
		}

		/* Find sparse data from the disk. */
		if (archive_entry_hardlink(entry) == NULL &&
		    (st->dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0)
			r = setup_sparse_from_disk(a, entry,
			    (HANDLE)_get_osfhandle(a->entry_fd));
	}

	/*
	 * EOF and FATAL are persistent at this layer.  By
	 * modifying the state, we guarantee that future calls to
	 * read a header or read data will fail.
	 */
	switch (r) {
	case ARCHIVE_EOF:
		a->archive.state = ARCHIVE_STATE_EOF;
		break;
	case ARCHIVE_OK:
	case ARCHIVE_WARN:
		a->entry_total = 0;
		if (archive_entry_filetype(entry) == AE_IFREG) {
			a->entry_remaining_bytes = archive_entry_size(entry);
			a->entry_eof = (a->entry_remaining_bytes == 0)? 1: 0;
			if (!a->entry_eof &&
			    setup_sparse(a, entry) != ARCHIVE_OK)
				return (ARCHIVE_FATAL);
		} else {
			a->entry_remaining_bytes = 0;
			a->entry_eof = 1;
		}
		a->archive.state = ARCHIVE_STATE_DATA;
		break;
	case ARCHIVE_RETRY:
		break;
	case ARCHIVE_FATAL:
		a->archive.state = ARCHIVE_STATE_FATAL;
		break;
	}

	return (r);
}

static int
setup_sparse(struct archive_read_disk *a, struct archive_entry *entry)
{
	struct tree *t = a->tree;
	int64_t length, offset;
	int i;

	t->sparse_count = archive_entry_sparse_reset(entry);
	if (t->sparse_count+1 > t->sparse_list_size) {
		free(t->sparse_list);
		t->sparse_list_size = t->sparse_count + 1;
		t->sparse_list = malloc(sizeof(t->sparse_list[0]) *
		    t->sparse_list_size);
		if (t->sparse_list == NULL) {
			t->sparse_list_size = 0;
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate data");
			a->archive.state = ARCHIVE_STATE_FATAL;
			return (ARCHIVE_FATAL);
		}
	}
	for (i = 0; i < t->sparse_count; i++) {
		archive_entry_sparse_next(entry, &offset, &length);
		t->sparse_list[i].offset = offset;
		t->sparse_list[i].length = length;
	}
	if (i == 0) {
		t->sparse_list[i].offset = 0;
		t->sparse_list[i].length = archive_entry_size(entry);
	} else {
		t->sparse_list[i].offset = archive_entry_size(entry);
		t->sparse_list[i].length = 0;
	}
	t->current_sparse = t->sparse_list;

	return (ARCHIVE_OK);
}

/*
 * Called by the client to mark the directory just returned from
 * tree_next() as needing to be visited.
 */
int
archive_read_disk_descend(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	struct tree *t = a->tree;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC, ARCHIVE_STATE_DATA,
	    "archive_read_disk_descend");

	if (t->visit_type != TREE_REGULAR || !t->descend) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Ignored the request descending the current object");
		return (ARCHIVE_WARN);
	}

	if (tree_current_is_physical_dir(t)) {
		tree_push(t, t->basename, t->full_path.s,
		    t->current_filesystem_id,
		    bhfi_dev(&(t->lst)), bhfi_ino(&(t->lst)),
		    &t->restore_time);
		t->stack->flags |= isDir;
	} else if (tree_current_is_dir(t)) {
		tree_push(t, t->basename, t->full_path.s,
		    t->current_filesystem_id,
		    bhfi_dev(&(t->st)), bhfi_ino(&(t->st)),
		    &t->restore_time);
		t->stack->flags |= isDirLink;
	}
	t->descend = 0;
	return (ARCHIVE_OK);
}

int
archive_read_disk_open(struct archive *_a, const char *pathname)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC,
	    ARCHIVE_STATE_NEW | ARCHIVE_STATE_CLOSED,
	    "archive_read_disk_open");
	archive_clear_error(&a->archive);

	if (a->tree != NULL)
		a->tree = tree_reopen(a->tree, pathname, a->restore_time);
	else
		a->tree = tree_open(pathname, a->symlink_mode,
		    a->restore_time);
	if (a->tree == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate tar data");
		a->archive.state = ARCHIVE_STATE_FATAL;
		return (ARCHIVE_FATAL);
	}
	a->archive.state = ARCHIVE_STATE_HEADER;
	a->entry_eof = 0;
	a->entry_fd = -1;

	return (ARCHIVE_OK);
}

/*
 * Return a current filesystem ID which is index of the filesystem entry
 * you've visited through archive_read_disk.
 */
int
archive_read_disk_current_filesystem(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC, ARCHIVE_STATE_DATA,
	    "archive_read_disk_current_filesystem");

	return (a->tree->current_filesystem_id);
}

static int
update_current_filesystem(struct archive_read_disk *a, int64_t dev)
{
	struct tree *t = a->tree;
	int i, fid;

	if (t->current_filesystem != NULL &&
	    t->current_filesystem->dev == dev)
		return (ARCHIVE_OK);

	for (i = 0; i < t->max_filesystem_id; i++) {
		if (t->filesystem_table[i].dev == dev) {
			/* There is the filesytem ID we've already generated. */
			t->current_filesystem_id = i;
			t->current_filesystem = &(t->filesystem_table[i]);
			return (ARCHIVE_OK);
		}
	}

	/*
	 * There is a new filesytem, we generate a new ID for.
	 */
	fid = t->max_filesystem_id++;
	if (t->max_filesystem_id > t->allocated_filesytem) {
		size_t s;

		s = t->max_filesystem_id * 2;
		t->filesystem_table = realloc(t->filesystem_table,
		    s * sizeof(*t->filesystem_table));
		if (t->filesystem_table == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate tar data");
			return (ARCHIVE_FATAL);
		}
		t->allocated_filesytem = s;
	}
	t->current_filesystem_id = fid;
	t->current_filesystem = &(t->filesystem_table[fid]);
	t->current_filesystem->dev = dev;

	return (setup_current_filesystem(a));
}

/*
 * Returns 1 if current filesystem is generated filesystem, 0 if it is not
 * or -1 if it is unknown.
 */
int
archive_read_disk_current_filesystem_is_synthetic(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC, ARCHIVE_STATE_DATA,
	    "archive_read_disk_current_filesystem");

	return (a->tree->current_filesystem->synthetic);
}

/*
 * Returns 1 if current filesystem is remote filesystem, 0 if it is not
 * or -1 if it is unknown.
 */
int
archive_read_disk_current_filesystem_is_remote(struct archive *_a)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;

	archive_check_magic(_a, ARCHIVE_READ_DISK_MAGIC, ARCHIVE_STATE_DATA,
	    "archive_read_disk_current_filesystem");

	return (a->tree->current_filesystem->remote);
}

/*
 * If symlink is broken, statfs or statvfs will fail.
 * Use its directory path instead.
 */
static wchar_t *
safe_path_for_statfs(struct tree *t)
{
	const wchar_t *path;
	wchar_t *cp, *p = NULL;

	path = tree_current_access_path(t);
	if (tree_current_stat(t) == NULL) {
		p = _wcsdup(path);
		cp = wcsrchr(p, '/');
		if (cp != NULL && wcslen(cp) >= 2) {
			cp[1] = '.';
			cp[2] = '\0';
			path = p;
		}
	} else
		p = _wcsdup(path);
	return (p);
}

/*
 * Get conditions of synthetic and remote on Windows
 */
static int
setup_current_filesystem(struct archive_read_disk *a)
{
	struct tree *t = a->tree;
	wchar_t vol[256];
	wchar_t *path;

	t->current_filesystem->synthetic = -1;/* Not supported */
	path = safe_path_for_statfs(t);
	if (!GetVolumePathNameW(path, vol, sizeof(vol)/sizeof(vol[0]))) {
		free(path);
		t->current_filesystem->remote = -1;
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "GetVolumePathName failed: %d", (int)GetLastError());
		return (ARCHIVE_FAILED);
	}
	free(path);
	switch (GetDriveTypeW(vol)) {
	case DRIVE_UNKNOWN:
	case DRIVE_NO_ROOT_DIR:
		t->current_filesystem->remote = -1;
		break;
	case DRIVE_REMOTE:
		t->current_filesystem->remote = 1;
		break;
	default:
		t->current_filesystem->remote = 0;
		break;
	}

	return (ARCHIVE_OK);
}

#define EPOC_TIME ARCHIVE_LITERAL_ULL(116444736000000000)
#define WINTIME(sec, nsec) ((Int32x32To64(sec, 10000000) + EPOC_TIME)\
	 + (((nsec)/1000)*10))

static int
close_and_restore_time(int fd, struct tree *t, struct restore_time *rt)
{
	HANDLE handle;
	ULARGE_INTEGER wintm;
	FILETIME fatime, fmtime;
	int r = 0;

	if (fd < 0 && AE_IFLNK == rt->filetype)
		return (0);

	/* Close a file descritor.
	 * It will not be used for SetFileTime() because it has been opened
	 * by a read only mode.
	 */
	if (fd >= 0)
		r = close(fd);
	if ((t->flags & needsRestoreTimes) == 0)
		return (r);

	handle = CreateFileW(rt->full_path, FILE_WRITE_ATTRIBUTES,
		    0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		errno = EINVAL;
		return (-1);
	}

	wintm.QuadPart = WINTIME(rt->atime, rt->atime_nsec);
	fatime.dwLowDateTime = wintm.LowPart;
	fatime.dwHighDateTime = wintm.HighPart;
	wintm.QuadPart = WINTIME(rt->mtime, rt->mtime_nsec);
	fmtime.dwLowDateTime = wintm.LowPart;
	fmtime.dwHighDateTime = wintm.HighPart;
	if (SetFileTime(handle, NULL, &fatime, &fmtime) == 0) {
		errno = EINVAL;
		r = -1;
	} else
		r = 0;
	CloseHandle(handle);
	return (r);
}

/*
 * Add a directory path to the current stack.
 */
static void
tree_push(struct tree *t, const wchar_t *path, const wchar_t *full_path,
    int filesystem_id, int64_t dev, int64_t ino, struct restore_time *rt)
{
	struct tree_entry *te;

	te = malloc(sizeof(*te));
	memset(te, 0, sizeof(*te));
	te->next = t->stack;
	te->parent = t->current;
	if (te->parent)
		te->depth = te->parent->depth + 1;
	t->stack = te;
	archive_string_init(&te->name);
	archive_wstrcpy(&te->name, path);
	archive_string_init(&te->full_path);
	archive_wstrcpy(&te->full_path, full_path);
	te->flags = needsDescent | needsOpen | needsAscent;
	te->filesystem_id = filesystem_id;
	te->dev = dev;
	te->ino = ino;
	te->dirname_length = t->dirname_length;
	te->full_path_dir_length = t->full_path_dir_length;
	te->restore_time.full_path = te->full_path.s;
	if (rt != NULL) {
		te->restore_time.mtime = rt->mtime;
		te->restore_time.mtime_nsec = rt->mtime_nsec;
		te->restore_time.atime = rt->atime;
		te->restore_time.atime_nsec = rt->atime_nsec;
		te->restore_time.filetype = rt->filetype;
	}
}

/*
 * Append a name to the current dir path.
 */
static void
tree_append(struct tree *t, const wchar_t *name, size_t name_length)
{
	size_t size_needed;

	t->path.s[t->dirname_length] = L'\0';
	t->path.length = t->dirname_length;
	/* Strip trailing '/' from name, unless entire name is "/". */
	while (name_length > 1 && name[name_length - 1] == L'/')
		name_length--;

	/* Resize pathname buffer as needed. */
	size_needed = name_length + t->dirname_length + 2;
	archive_wstring_ensure(&t->path, size_needed);
	/* Add a separating '/' if it's needed. */
	if (t->dirname_length > 0 &&
	    t->path.s[archive_strlen(&t->path)-1] != L'/')
		archive_wstrappend_wchar(&t->path, L'/');
	t->basename = t->path.s + archive_strlen(&t->path);
	archive_wstrncat(&t->path, name, name_length);
	t->restore_time.full_path = t->basename;
	if (t->full_path_dir_length > 0) {
		t->full_path.s[t->full_path_dir_length] = L'\0';
		t->full_path.length = t->full_path_dir_length;
		size_needed = name_length + t->full_path_dir_length + 2;
		archive_wstring_ensure(&t->full_path, size_needed);
		/* Add a separating '\' if it's needed. */
		if (t->full_path.s[archive_strlen(&t->full_path)-1] != L'\\')
			archive_wstrappend_wchar(&t->full_path, L'\\');
		archive_wstrncat(&t->full_path, name, name_length);
		t->restore_time.full_path = t->full_path.s;
	}
}

/*
 * Open a directory tree for traversal.
 */
static struct tree *
tree_open(const char *path, int symlink_mode, int restore_time)
{
	struct tree *t;

	t = malloc(sizeof(*t));
	memset(t, 0, sizeof(*t));
	archive_string_init(&(t->full_path));
	archive_string_init(&t->path);
	archive_wstring_ensure(&t->path, 15);
	archive_string_init(&t->temp);
	t->initial_symlink_mode = symlink_mode;
	return (tree_reopen(t, path, restore_time));
}

static struct tree *
tree_reopen(struct tree *t, const char *path, int restore_time)
{
	struct archive_wstring ws;
	wchar_t *pathname, *p, *base;

	t->flags = (restore_time)?needsRestoreTimes:0;
	t->visit_type = 0;
	t->tree_errno = 0;
	t->full_path_dir_length = 0;
	t->dirname_length = 0;
	t->depth = 0;
	t->descend = 0;
	t->current = NULL;
	t->d = INVALID_DIR_HANDLE;
	t->symlink_mode = t->initial_symlink_mode;
	archive_string_empty(&(t->full_path));
	archive_string_empty(&t->path);

	/* Get wchar_t strings from char strings. */
	archive_string_init(&ws);
	if (archive_wstring_append_from_mbs(NULL, &ws,
	    path, strlen(path)) != 0)
		goto failed;
	pathname = ws.s;
	/* Get a full-path-name. */
	p = __la_win_permissive_name_w(pathname);
	if (p == NULL)
		goto failed;
	archive_wstrcpy(&(t->full_path), p);
	free(p);

	/* Convert path separators from '\' to '/' */
	for (p = pathname; *p != L'\0'; ++p) {
		if (*p == L'\\')
			*p = L'/';
	}
	base = pathname;

	/* First item is set up a lot like a symlink traversal. */
	/* printf("Looking for wildcard in %s\n", path); */
	/* TODO: wildcard detection here screws up on \\?\c:\ UNC names */
	if (wcschr(base, L'*') || wcschr(base, L'?')) {
		// It has a wildcard in it...
		// Separate the last element.
		p = wcsrchr(base, L'/');
		if (p != NULL) {
			*p = L'\0';
			tree_append(t, base, p - base);
			t->dirname_length = archive_strlen(&t->path);
			base = p + 1;
		}
		p = wcsrchr(t->full_path.s, L'\\');
		if (p != NULL) {
			*p = L'\0';
			t->full_path.length = wcslen(t->full_path.s);
			t->full_path_dir_length = archive_strlen(&t->full_path);
		}
	}
	tree_push(t, base, t->full_path.s, 0, 0, 0, NULL);
	archive_wstring_free(&ws);
	t->stack->flags = needsFirstVisit;
	return (t);
failed:
	archive_wstring_free(&ws);
	tree_free(t);
	return (NULL);
}

static int
tree_descent(struct tree *t)
{
	t->dirname_length = archive_strlen(&t->path);
	t->full_path_dir_length = archive_strlen(&t->full_path);
	t->depth++;
	return (0);
}

/*
 * We've finished a directory; ascend back to the parent.
 */
static int
tree_ascend(struct tree *t)
{
	struct tree_entry *te;

	te = t->stack;
	t->depth--;
	close_and_restore_time(-1, t, &te->restore_time);
	return (0);
}

/*
 * Pop the working stack.
 */
static void
tree_pop(struct tree *t)
{
	struct tree_entry *te;

	t->full_path.s[t->full_path_dir_length] = L'\0';
	t->full_path.length = t->full_path_dir_length;
	t->path.s[t->dirname_length] = L'\0';
	t->path.length = t->dirname_length;
	if (t->stack == t->current && t->current != NULL)
		t->current = t->current->parent;
	te = t->stack;
	t->stack = te->next;
	t->dirname_length = te->dirname_length;
	t->basename = t->path.s + t->dirname_length;
	t->full_path_dir_length = te->full_path_dir_length;
	while (t->basename[0] == L'/')
		t->basename++;
	archive_wstring_free(&te->name);
	archive_wstring_free(&te->full_path);
	free(te);
}

/*
 * Get the next item in the tree traversal.
 */
static int
tree_next(struct tree *t)
{
	int r;

	while (t->stack != NULL) {
		/* If there's an open dir, get the next entry from there. */
		if (t->d != INVALID_DIR_HANDLE) {
			r = tree_dir_next_windows(t, NULL);
			if (r == 0)
				continue;
			return (r);
		}

		if (t->stack->flags & needsFirstVisit) {
			wchar_t *d = t->stack->name.s;
			t->stack->flags &= ~needsFirstVisit;
			if (wcschr(d, L'*') || wcschr(d, L'?')) {
				r = tree_dir_next_windows(t, d);
				if (r == 0)
					continue;
				return (r);
			} else {
				HANDLE h = FindFirstFileW(d, &t->_findData);
				if (h == INVALID_DIR_HANDLE) {
					t->tree_errno = errno;
					t->visit_type = TREE_ERROR_DIR;
					return (t->visit_type);
				}
				t->findData = &t->_findData;
				FindClose(h);
			}
			/* Top stack item needs a regular visit. */
			t->current = t->stack;
			tree_append(t, t->stack->name.s,
			    archive_strlen(&(t->stack->name)));
			//t->dirname_length = t->path_length;
			//tree_pop(t);
			t->stack->flags &= ~needsFirstVisit;
			return (t->visit_type = TREE_REGULAR);
		} else if (t->stack->flags & needsDescent) {
			/* Top stack item is dir to descend into. */
			t->current = t->stack;
			tree_append(t, t->stack->name.s,
			    archive_strlen(&(t->stack->name)));
			t->stack->flags &= ~needsDescent;
			r = tree_descent(t);
			if (r != 0) {
				tree_pop(t);
				t->visit_type = r;
			} else
				t->visit_type = TREE_POSTDESCENT;
			return (t->visit_type);
		} else if (t->stack->flags & needsOpen) {
			t->stack->flags &= ~needsOpen;
			r = tree_dir_next_windows(t, L"*");
			if (r == 0)
				continue;
			return (r);
		} else if (t->stack->flags & needsAscent) {
		        /* Top stack item is dir and we're done with it. */
			r = tree_ascend(t);
			tree_pop(t);
			t->visit_type = r != 0 ? r : TREE_POSTASCENT;
			return (t->visit_type);
		} else {
			/* Top item on stack is dead. */
			tree_pop(t);
			t->flags &= ~hasLstat;
			t->flags &= ~hasStat;
		}
	}
	return (t->visit_type = 0);
}

static int
tree_dir_next_windows(struct tree *t, const wchar_t *pattern)
{
	const wchar_t *name;
	size_t namelen;
	int r;

	for (;;) {
		if (pattern != NULL) {
			struct archive_wstring pt;

			archive_string_init(&pt);
			archive_wstring_ensure(&pt,
			    archive_strlen(&(t->full_path))
			      + 2 + wcslen(pattern));
			archive_wstring_copy(&pt, &(t->full_path));
			archive_wstrappend_wchar(&pt, L'\\');
			archive_wstrcat(&pt, pattern);
			t->d = FindFirstFileW(pt.s, &t->_findData);
			archive_wstring_free(&pt);
			if (t->d == INVALID_DIR_HANDLE) {
				r = tree_ascend(t); /* Undo "chdir" */
				tree_pop(t);
				t->tree_errno = errno;
				t->visit_type = r != 0 ? r : TREE_ERROR_DIR;
				return (t->visit_type);
			}
			t->findData = &t->_findData;
			pattern = NULL;
		} else if (!FindNextFileW(t->d, &t->_findData)) {
			FindClose(t->d);
			t->d = INVALID_DIR_HANDLE;
			t->findData = NULL;
			return (0);
		}
		name = t->findData->cFileName;
		namelen = wcslen(name);
		t->flags &= ~hasLstat;
		t->flags &= ~hasStat;
		if (name[0] == L'.' && name[1] == L'\0')
			continue;
		if (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0')
			continue;
		tree_append(t, name, namelen);
		return (t->visit_type = TREE_REGULAR);
	}
}

static void
fileTimeToUtc(const FILETIME *filetime, time_t *time, long *ns)
{
	ULARGE_INTEGER utc;

	utc.HighPart = filetime->dwHighDateTime;
	utc.LowPart  = filetime->dwLowDateTime;
	if (utc.QuadPart >= EPOC_TIME) {
		utc.QuadPart -= EPOC_TIME;
		/* milli seconds base */
		*time = (time_t)(utc.QuadPart / 10000000);
		/* nano seconds base */
		*ns = (long)(utc.QuadPart % 10000000) * 100;
	} else {
		*time = 0;
		*ns = 0;
	}
}

static void
entry_copy_bhfi(struct archive_entry *entry, const wchar_t *path,
	const WIN32_FIND_DATAW *findData,
	const BY_HANDLE_FILE_INFORMATION *bhfi)
{
	time_t secs;
	long nsecs;
	mode_t mode;

	fileTimeToUtc(&bhfi->ftLastAccessTime, &secs, &nsecs);
	archive_entry_set_atime(entry, secs, nsecs);
	fileTimeToUtc(&bhfi->ftLastWriteTime, &secs, &nsecs);
	archive_entry_set_mtime(entry, secs, nsecs);
	fileTimeToUtc(&bhfi->ftCreationTime, &secs, &nsecs);
	archive_entry_set_birthtime(entry, secs, nsecs);
	archive_entry_set_ctime(entry, secs, nsecs);
	archive_entry_set_dev(entry, bhfi_dev(bhfi));
	archive_entry_set_ino64(entry, bhfi_ino(bhfi));
	if (bhfi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		archive_entry_set_nlink(entry, bhfi->nNumberOfLinks + 1);
	else
		archive_entry_set_nlink(entry, bhfi->nNumberOfLinks);
	archive_entry_set_size(entry,
	    (((int64_t)bhfi->nFileSizeHigh) << 32)
	    + bhfi->nFileSizeLow);
	archive_entry_set_uid(entry, 0);
	archive_entry_set_gid(entry, 0);
	archive_entry_set_rdev(entry, 0);

	mode = S_IRUSR | S_IRGRP | S_IROTH;
	if ((bhfi->dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0)
		mode |= S_IWUSR | S_IWGRP | S_IWOTH;
	if ((bhfi->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
	    findData != NULL &&
	    findData->dwReserved0 == IO_REPARSE_TAG_SYMLINK)
		mode |= S_IFLNK;
	else if (bhfi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;
	else {
		const wchar_t *p;

		mode |= S_IFREG;
		p = wcsrchr(path, L'.');
		if (p != NULL && wcslen(p) == 4) {
			switch (p[1]) {
			case L'B': case L'b':
				if ((p[2] == L'A' || p[2] == L'a' ) &&
				    (p[3] == L'T' || p[3] == L't' ))
					mode |= S_IXUSR | S_IXGRP | S_IXOTH;
				break;
			case L'C': case L'c':
				if (((p[2] == L'M' || p[2] == L'm' ) &&
				    (p[3] == L'D' || p[3] == L'd' )) ||
				    ((p[2] == L'M' || p[2] == L'm' ) &&
				    (p[3] == L'D' || p[3] == L'd' )))
					mode |= S_IXUSR | S_IXGRP | S_IXOTH;
				break;
			case L'E': case L'e':
				if ((p[2] == L'X' || p[2] == L'x' ) &&
				    (p[3] == L'E' || p[3] == L'e' ))
					mode |= S_IXUSR | S_IXGRP | S_IXOTH;
				break;
			default:
				break;
			}
		}
	}
	archive_entry_set_mode(entry, mode);
}

static void
tree_archive_entry_copy_bhfi(struct archive_entry *entry, struct tree *t,
	const BY_HANDLE_FILE_INFORMATION *bhfi)
{
	entry_copy_bhfi(entry, tree_current_path(t), t->findData, bhfi);
}

static int
tree_current_file_information(struct tree *t, BY_HANDLE_FILE_INFORMATION *st,
 int sim_lstat)
{
	HANDLE h;
	int r;
	DWORD flag = FILE_FLAG_BACKUP_SEMANTICS;
	
	if (sim_lstat && tree_current_is_physical_link(t))
		flag |= FILE_FLAG_OPEN_REPARSE_POINT;
	h = CreateFileW(tree_current_access_path(t), 0, 0, NULL,
	    OPEN_EXISTING, flag, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return (0);
	r = GetFileInformationByHandle(h, st);
	CloseHandle(h);
	return (r);
}

/*
 * Get the stat() data for the entry just returned from tree_next().
 */
static const BY_HANDLE_FILE_INFORMATION *
tree_current_stat(struct tree *t)
{
	if (!(t->flags & hasStat)) {
		if (!tree_current_file_information(t, &t->st, 0))
			return NULL;
		t->flags |= hasStat;
	}
	return (&t->st);
}

/*
 * Get the lstat() data for the entry just returned from tree_next().
 */
static const BY_HANDLE_FILE_INFORMATION *
tree_current_lstat(struct tree *t)
{
	if (!(t->flags & hasLstat)) {
		if (!tree_current_file_information(t, &t->lst, 1))
			return NULL;
		t->flags |= hasLstat;
	}
	return (&t->lst);
}

/*
 * Test whether current entry is a dir or link to a dir.
 */
static int
tree_current_is_dir(struct tree *t)
{
	if (t->findData)
		return (t->findData->dwFileAttributes
		    & FILE_ATTRIBUTE_DIRECTORY);
	return (0);
}

/*
 * Test whether current entry is a physical directory.  Usually, we
 * already have at least one of stat() or lstat() in memory, so we
 * use tricks to try to avoid an extra trip to the disk.
 */
static int
tree_current_is_physical_dir(struct tree *t)
{
	if (tree_current_is_physical_link(t))
		return (0);
	return (tree_current_is_dir(t));
}

/*
 * Test whether current entry is a symbolic link.
 */
static int
tree_current_is_physical_link(struct tree *t)
{
	if (t->findData)
		return ((t->findData->dwFileAttributes
			        & FILE_ATTRIBUTE_REPARSE_POINT) &&
			(t->findData->dwReserved0
			    == IO_REPARSE_TAG_SYMLINK));
	return (0);
}

/*
 * Test whether the same file has been in the tree as its parent.
 */
static int
tree_target_is_same_as_parent(struct tree *t,
    const BY_HANDLE_FILE_INFORMATION *st)
{
	struct tree_entry *te;
	int64_t dev = bhfi_dev(st);
	int64_t ino = bhfi_ino(st);

	for (te = t->current->parent; te != NULL; te = te->parent) {
		if (te->dev == dev && te->ino == ino)
			return (1);
	}
	return (0);
}

/*
 * Return the access path for the entry just returned from tree_next().
 */
static const wchar_t *
tree_current_access_path(struct tree *t)
{
	return (t->full_path.s);
}

/*
 * Return the full path for the entry just returned from tree_next().
 */
static const wchar_t *
tree_current_path(struct tree *t)
{
	return (t->path.s);
}

/*
 * Terminate the traversal.
 */
static void
tree_close(struct tree *t)
{
	/* Close the handle of FindFirstFileW */
	if (t->d != INVALID_DIR_HANDLE) {
		FindClose(t->d);
		t->d = INVALID_DIR_HANDLE;
		t->findData = NULL;
	}
	/* Release anything remaining in the stack. */
	while (t->stack != NULL)
		tree_pop(t);
}

/*
 * Release any resources.
 */
static void
tree_free(struct tree *t)
{
	if (t == NULL)
		return;
	archive_wstring_free(&t->path);
	archive_wstring_free(&t->full_path);
	archive_string_free(&t->temp);
	free(t->sparse_list);
	free(t->filesystem_table);
	free(t);
}


/*
 * Populate the archive_entry with metadata from the disk.
 */
int
archive_read_disk_entry_from_file(struct archive *_a,
    struct archive_entry *entry, int fd, const struct stat *st)
{
	struct archive_read_disk *a = (struct archive_read_disk *)_a;
	const wchar_t *path;
	const char *name;
	HANDLE h;
	BY_HANDLE_FILE_INFORMATION bhfi;
	DWORD fileAttributes;
	int r;

	archive_clear_error(_a);
	name = archive_entry_sourcepath(entry);
	if (name == NULL)
		name = archive_entry_pathname(entry);
	path = __la_win_permissive_name(name);

	if (st == NULL) {
		/*
		 * Get metadata through GetFileInformationByHandle().
		 */
		if (fd >= 0) {
			h = (HANDLE)_get_osfhandle(fd);
			r = GetFileInformationByHandle(h, &bhfi);
			if (r == 0) {
				archive_set_error(&a->archive, GetLastError(),
				    "Can't GetFileInformationByHandle");
				return (ARCHIVE_FAILED);
			}
			entry_copy_bhfi(entry, path, NULL, &bhfi);
		} else {
			WIN32_FIND_DATAW findData;
			DWORD flag, desiredAccess;
	
			h = FindFirstFileW(path, &findData);
			if (h == INVALID_DIR_HANDLE) {
				archive_set_error(&a->archive, GetLastError(),
				    "Can't FindFirstFileW");
				return (ARCHIVE_FAILED);
			}
			FindClose(h);

			flag = FILE_FLAG_BACKUP_SEMANTICS;
			if (!a->follow_symlinks &&
			    (findData.dwFileAttributes
			      & FILE_ATTRIBUTE_REPARSE_POINT) &&
				  (findData.dwReserved0 == IO_REPARSE_TAG_SYMLINK)) {
				flag |= FILE_FLAG_OPEN_REPARSE_POINT;
				desiredAccess = 0;
			} else
				desiredAccess = GENERIC_READ;

			h = CreateFileW(path, desiredAccess, 0, NULL,
			    OPEN_EXISTING, flag, NULL);
			if (h == INVALID_HANDLE_VALUE) {
				archive_set_error(&a->archive,
				    GetLastError(),
				    "Can't CreateFileW");
				return (ARCHIVE_FAILED);
			}
			r = GetFileInformationByHandle(h, &bhfi);
			if (r == 0) {
				archive_set_error(&a->archive,
				    GetLastError(),
				    "Can't GetFileInformationByHandle");
				CloseHandle(h);
				return (ARCHIVE_FAILED);
			}
			entry_copy_bhfi(entry, path, &findData, &bhfi);
		}
		fileAttributes = bhfi.dwFileAttributes;
	} else {
		archive_entry_copy_stat(entry, st);
		h = INVALID_DIR_HANDLE;
	}

	/* Lookup uname/gname */
	name = archive_read_disk_uname(_a, archive_entry_uid(entry));
	if (name != NULL)
		archive_entry_copy_uname(entry, name);
	name = archive_read_disk_gname(_a, archive_entry_gid(entry));
	if (name != NULL)
		archive_entry_copy_gname(entry, name);

	/*
	 * Can this file be sparse file ?
	 */
	if (archive_entry_filetype(entry) != AE_IFREG
	    || archive_entry_size(entry) <= 0
		|| archive_entry_hardlink(entry) != NULL) {
		if (h != INVALID_HANDLE_VALUE && fd < 0)
			CloseHandle(h);
		return (ARCHIVE_OK);
	}

	if (h == INVALID_HANDLE_VALUE) {
		if (fd >= 0) {
			h = (HANDLE)_get_osfhandle(fd);
		} else {
			h = CreateFileW(path, GENERIC_READ, 0, NULL,
			    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
			if (h == INVALID_HANDLE_VALUE) {
				archive_set_error(&a->archive, GetLastError(),
				    "Can't CreateFileW");
				return (ARCHIVE_FAILED);
			}
		}
		r = GetFileInformationByHandle(h, &bhfi);
		if (r == 0) {
			archive_set_error(&a->archive, GetLastError(),
			    "Can't GetFileInformationByHandle");
			if (h != INVALID_HANDLE_VALUE && fd < 0)
				CloseHandle(h);
			return (ARCHIVE_FAILED);
		}
		fileAttributes = bhfi.dwFileAttributes;
	}

	/* Sparse file must be set a mark, FILE_ATTRIBUTE_SPARSE_FILE */
	if ((fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0) {
		if (fd < 0)
			CloseHandle(h);
		return (ARCHIVE_OK);
	}

	r = setup_sparse_from_disk(a, entry, h);
	if (fd < 0)
		CloseHandle(h);

	return (r);
}

/*
 * Windows sparse interface.
 */
#if defined(__MINGW32__) && !defined(FSCTL_QUERY_ALLOCATED_RANGES)
#define FSCTL_QUERY_ALLOCATED_RANGES 0x940CF
typedef struct {
	LARGE_INTEGER FileOffset;
	LARGE_INTEGER Length;
} FILE_ALLOCATED_RANGE_BUFFER;
#endif

static int
setup_sparse_from_disk(struct archive_read_disk *a,
    struct archive_entry *entry, HANDLE handle)
{
	FILE_ALLOCATED_RANGE_BUFFER range, *outranges = NULL;
	size_t outranges_size;
	int64_t entry_size = archive_entry_size(entry);
	int exit_sts = ARCHIVE_OK;

	range.FileOffset.QuadPart = 0;
	range.Length.QuadPart = entry_size;
	outranges_size = 2048;
	outranges = (FILE_ALLOCATED_RANGE_BUFFER *)malloc(outranges_size);
	if (outranges == NULL) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			"Couldn't allocate memory");
		exit_sts = ARCHIVE_FATAL;
		goto exit_setup_sparse;
	}

	for (;;) {
		DWORD retbytes;
		BOOL ret;

		for (;;) {
			ret = DeviceIoControl(handle,
			    FSCTL_QUERY_ALLOCATED_RANGES,
			    &range, sizeof(range), outranges,
			    outranges_size, &retbytes, NULL);
			if (ret == 0 && GetLastError() == ERROR_MORE_DATA) {
				free(outranges);
				outranges_size *= 2;
				outranges = (FILE_ALLOCATED_RANGE_BUFFER *)
				    malloc(outranges_size);
				if (outranges == NULL) {
					archive_set_error(&a->archive,
					    ARCHIVE_ERRNO_MISC,
					    "Couldn't allocate memory");
					exit_sts = ARCHIVE_FATAL;
					goto exit_setup_sparse;
				}
				continue;
			} else
				break;
		}
		if (ret != 0) {
			if (retbytes > 0) {
				DWORD i, n;

				n = retbytes / sizeof(outranges[0]);
				if (n == 1 &&
				    outranges[0].FileOffset.QuadPart == 0 &&
				    outranges[0].Length.QuadPart == entry_size)
					break;/* This is not sparse. */
				for (i = 0; i < n; i++)
					archive_entry_sparse_add_entry(entry,
					    outranges[i].FileOffset.QuadPart,
						outranges[i].Length.QuadPart);
				range.FileOffset.QuadPart =
				    outranges[n-1].FileOffset.QuadPart
				    + outranges[n-1].Length.QuadPart;
				range.Length.QuadPart =
				    entry_size - range.FileOffset.QuadPart;
				if (range.Length.QuadPart > 0)
					continue;
			} else {
				/* The remaining data is hole. */
				archive_entry_sparse_add_entry(entry,
				    range.FileOffset.QuadPart,
				    range.Length.QuadPart);
			}
			break;
		} else {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "DeviceIoControl Failed: %lu", GetLastError());
			exit_sts = ARCHIVE_FAILED;
			goto exit_setup_sparse;
		}
	}
exit_setup_sparse:
	free(outranges);

	return (exit_sts);
}

#else
/* Suppress the 'no symbols in this file' warning on some compilers by
 * defining a useless function on non-Windows systems.
 */
void __archive_read_disk_windows_useless_function(void) {
}
#endif
