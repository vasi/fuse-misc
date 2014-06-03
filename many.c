// (C) 2012 Dave Vasilevsky <dave@vasilevsky.ca>
// Licensing: GPL v2, see the COPYING file

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>

static const char file_content[] = "Hello World!\n";
static const size_t file_size      = sizeof(file_content)/sizeof(char) - 1;

static const size_t branch = 64;
static size_t name_size = 3;
static const size_t total = 5 * 1000 * 1000;

static int num_parse(char *name) {
	if (!name)
		return -1;
	if (strlen(name) != name_size)
		return -1;
	
	char *end = NULL;
	int ret = strtol(name, &end, 10);
	if (*end)
		return -1;
	if (ret >= branch)
		return -1;
	return ret;
}

static int64_t child(int64_t dnum, int fnum) {
	return branch * dnum + 1 + fnum;
}

static char *split_path(char *path, size_t *n) {
	int i;
	for (i = *n - 1; ; --i) {
		if (i == 0 || path[i] == '/') {
			path[i] = '\0';
			*n = i;
			return path + i + 1;
		}
	}
}

static int64_t inum_inner(char *path, size_t n) {
	if (!*path || strcmp(path, "/") == 0)
		return 0;
	
	char *base = split_path(path, &n);
//	fprintf(stderr, "DIR: %s\nBASE: %s\n", path, base);
	uint64_t dnum = inum_inner(path, n);
	if (dnum == -1)
		return -1;	
	int fnum = num_parse(base);
	if (fnum == -1)
		return -1;
	
	return child(dnum, fnum);
}

static int64_t inum(const char *path) {
//	fprintf(stderr, "\nLOOKUP: %s\n", path);

	static char s[PATH_MAX];
	strcpy(s, path);
	return inum_inner(s, strlen(s));
}

static int children(int64_t n) {
	int64_t first = child(n, 0);
	if (first >= total)
		return 0;
	if (first + branch <= total)
		return branch;
	return total - first;
}

static int many_getattr(const char *path, struct stat *stbuf) {
	int64_t n = inum(path);
	if (n == -1)
		return -ENOENT;
	
	if (children(n)) {
		stbuf->st_mode = S_IFDIR | 0555;
	} else {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_size = file_size;
	}
	return 0;
}

static int many_open(const char *path, struct fuse_file_info *fi) {
	int64_t n = inum(path);
	if (n == -1)
		return -ENOENT;
	if (children(n))
		return -EISDIR;
	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES; // read-only
	return 0;
}

static int many_read(const char *path, char *buf, size_t size,
		off_t offset, struct fuse_file_info *fi) {
	if (offset >= file_size)
		return 0;
	if (offset + size > file_size)
		size = file_size - offset;
	memcpy(buf, file_content + offset, size);
	return size;
}

static int many_readdir(const char *path, void *buf,
		fuse_fill_dir_t filler, off_t offset,
		struct fuse_file_info *fi) {
	int64_t n = inum(path);
	if (n == -1)
		return -ENOENT;
	int c = children(n);
	if (!c)
		return -ENOTDIR;
	
	int i;
	static char s[PATH_MAX];
	for (i = 0; i < c; ++i) {
		sprintf(s, "%0*d", (int)name_size, i);
		filler(buf, s, NULL, 0);
	}
	return 0;
}

static struct fuse_operations many_fsops = {
	.getattr	= many_getattr,
	.open		= many_open,
	.read		= many_read,
	.readdir	= many_readdir,
};

int main(int argc, char **argv) {
	return fuse_main(argc, argv, &many_fsops, NULL);
}
