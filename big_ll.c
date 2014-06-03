// (C) 2012 Dave Vasilevsky <dave@vasilevsky.ca>
// Licensing: GPL v2, see the COPYING file

#define FUSE_USE_VERSION 26

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

const off_t block_size = 1024 * 128;
const off_t total_size = 1024LL * 1024 * 1024 * 1024 * 3 / 2;

static const char *hello_str = "Hello World!\n";
static const char *hello_name = "hello";

static int hello_stat(fuse_ino_t ino, struct stat *stbuf)
{
	stbuf->st_ino = ino;
	switch (ino) {
	case 1:
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		break;

	case 2:
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = total_size;
		break;

	default:
		return -1;
	}
	return 0;
}

static void hello_ll_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
	struct stat stbuf;

	(void) fi;

	memset(&stbuf, 0, sizeof(stbuf));
	if (hello_stat(ino, &stbuf) == -1)
		fuse_reply_err(req, ENOENT);
	else
		fuse_reply_attr(req, &stbuf, 1.0);
}

static void hello_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e;

	if (parent != 1 || strcmp(name, hello_name) != 0)
		fuse_reply_err(req, ENOENT);
	else {
		memset(&e, 0, sizeof(e));
		e.ino = 2;
		e.attr_timeout = 1.0;
		e.entry_timeout = 1.0;
		hello_stat(e.ino, &e.attr);

		fuse_reply_entry(req, &e);
	}
}

struct dirbuf {
	char *p;
	size_t size;
};

static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name,
		       fuse_ino_t ino)
{
	struct stat stbuf;
	size_t oldsize = b->size;
	b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
	b->p = (char *) realloc(b->p, b->size);
	memset(&stbuf, 0, sizeof(stbuf));
	stbuf.st_ino = ino;
	fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
			  b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
			     off_t off, size_t maxsize)
{
	if (off < bufsize)
		return fuse_reply_buf(req, buf + off,
				      min(bufsize - off, maxsize));
	else
		return fuse_reply_buf(req, NULL, 0);
}

static void hello_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			     off_t off, struct fuse_file_info *fi)
{
	(void) fi;

	if (ino != 1)
		fuse_reply_err(req, ENOTDIR);
	else {
		struct dirbuf b;

		memset(&b, 0, sizeof(b));
		dirbuf_add(req, &b, ".", 1);
		dirbuf_add(req, &b, "..", 1);
		dirbuf_add(req, &b, hello_name, 2);
		reply_buf_limited(req, b.p, b.size, off, size);
		free(b.p);
	}
}

static void hello_ll_open(fuse_req_t req, fuse_ino_t ino,
			  struct fuse_file_info *fi)
{
	if (ino != 2)
		fuse_reply_err(req, EISDIR);
	else if ((fi->flags & 3) != O_RDONLY)
		fuse_reply_err(req, EACCES);
	else
		fuse_reply_open(req, fi);
}

// Get the data of one block
static char *get_block(fuse_req_t req, uint64_t i) {
	char *buf = (char*)fuse_req_userdata(req);
	
	// Make it different from other blocks, to defeat any dedup
	*(uint64_t*)buf = i;
	
	return buf;
}

static void hello_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			  off_t off, struct fuse_file_info *fi)
{
	char *buf;
	size_t remain = size;
	
	(void) fi;

	assert(ino == 2);
	
	//fprintf(stderr, "read: %15llx\n", off);
	if (off > total_size)
		off = total_size;
	if (size > total_size - off)
		size = total_size - off;
	
	if (!(buf = malloc(size))) {
		fuse_reply_err(req, ENOMEM);
		return;
	}
	
	while (remain) {
		char *block;
		
		uint64_t block_idx = off / block_size;
		off_t start = off % block_size;
		
		off_t avail = block_size - start;		
		size_t take = remain > avail ? avail : remain;
		
		block = get_block(req, block_idx);
		memcpy(buf, block + start, take);
		
		off += take;
		remain -= take;
	}
	
	fuse_reply_buf(req, buf, size);
	free(buf);
}

static struct fuse_lowlevel_ops hello_ll_oper = {
	.lookup		= hello_ll_lookup,
	.getattr	= hello_ll_getattr,
	.readdir	= hello_ll_readdir,
	.open		= hello_ll_open,
	.read		= hello_ll_read,
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_chan *ch;
	char *buf, *mountpoint, *base;
	int err = -1;
	
	base = NULL;
	struct fuse_opt opts[] = {
		{ "--base=%s", 0, 0 },
		FUSE_OPT_END,
	};
	if (fuse_opt_parse(&args, &base, opts, NULL) == -1) {
		fprintf(stderr, "Bad opts\n");
		exit(-2);
	}
	
	// Initialize our block data
	if (!(buf = calloc(1, block_size))) {
		fprintf(stderr, "Out of mem\n");
		exit(-1);
	}
	if (base) { // Given a filename, 
		int fd = open(base, O_RDONLY);
		if (fd == -1) {
			fprintf(stderr, "Can't open base file\n");
			exit(-1);
		}
		if (read(fd, buf, block_size) <= 0) {
			fprintf(stderr, "Bad read\n");
			exit(-1);
		}
	}	
	
	if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
	    (ch = fuse_mount(mountpoint, &args)) != NULL) {
		struct fuse_session *se;

		se = fuse_lowlevel_new(&args, &hello_ll_oper,
				       sizeof(hello_ll_oper), buf);
		if (se != NULL && fuse_daemonize(1) != -1) {
			if (fuse_set_signal_handlers(se) != -1) {
				fuse_session_add_chan(se, ch);
				err = fuse_session_loop(se);
				fuse_remove_signal_handlers(se);
				fuse_session_remove_chan(ch);
			}
			fuse_session_destroy(se);
		}
		fuse_unmount(mountpoint, ch);
	}
	fuse_opt_free_args(&args);

	return err ? 1 : 0;
}
