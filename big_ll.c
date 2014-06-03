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
#include <stddef.h>

typedef struct {
	char *basebuf;
	off_t block_size, total_size;
} big_ctx;

static const char *hello_str = "Hello World!\n";
static const char *hello_name = "hello";

static int hello_stat(fuse_ino_t ino, struct stat *stbuf, off_t total_size)
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

static void big_ll_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
	struct stat stbuf;
	big_ctx *ctx = (big_ctx*)fuse_req_userdata(req);
	
	(void) fi;

	memset(&stbuf, 0, sizeof(stbuf));
	if (hello_stat(ino, &stbuf, ctx->total_size) == -1)
		fuse_reply_err(req, ENOENT);
	else
		fuse_reply_attr(req, &stbuf, 1.0);
}

static void big_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e;
	big_ctx *ctx = (big_ctx*)fuse_req_userdata(req);

	if (parent != 1 || strcmp(name, hello_name) != 0)
		fuse_reply_err(req, ENOENT);
	else {
		memset(&e, 0, sizeof(e));
		e.ino = 2;
		e.attr_timeout = 1.0;
		e.entry_timeout = 1.0;
		hello_stat(e.ino, &e.attr, ctx->total_size);

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

static void big_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
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

static void big_ll_open(fuse_req_t req, fuse_ino_t ino,
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
static char *get_block(big_ctx *ctx, uint64_t i) {
	char *buf = ctx->basebuf;
	
	// Make it different from other blocks, to defeat any dedup
	*(uint64_t*)buf = i;
	
	return buf;
}

static void big_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			  off_t off, struct fuse_file_info *fi)
{
	char *buf, *pos;
	big_ctx *ctx;
	size_t remain;
	
	(void) fi;
	assert(ino == 2);
	
	ctx = (big_ctx*)fuse_req_userdata(req);
	
	if (off > ctx->total_size)
		off = ctx->total_size;
	if (size > ctx->total_size - off)
		size = ctx->total_size - off;
	if (size == 0) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}
	
	
	if (!(buf = malloc(size))) {
		fuse_reply_err(req, ENOMEM);
		return;
	}
	
	remain = size;
	pos = buf;
	while (remain) {
		char *block;
		
		uint64_t block_idx = off / ctx->block_size;
		off_t start = off % ctx->block_size;
		
		off_t avail = ctx->block_size - start;		
		size_t take = remain > avail ? avail : remain;
		
		block = get_block(ctx, block_idx);
		memcpy(pos, block + start, take);
		
		off += take;
		remain -= take;
		pos += take;
	}
	
	fuse_reply_buf(req, buf, size);
	free(buf);
}

static off_t parse_size(char *sizespec, char *dflt) {
	long long ret;
	char *endptr;
	size_t slen;
	
	if (!sizespec)
		sizespec = dflt;
	
	errno = 0;
	ret = strtoll(sizespec, &endptr, 10);
	
	if (endptr != sizespec && errno == 0) {
		slen = strlen(endptr);
		if (slen == 0)
			return ret;
		if (slen == 1 || (slen == 2 || tolower(endptr[1]) == 'b')) {
			char *sufs = "kmgtp";
			char c = tolower(endptr[0]);
			char *suf = strchr(sufs, c);
			if (suf) {
				for (; suf >= sufs; --suf)
					ret *= 1024;
				return ret;
			}
		}
	}

	fprintf(stderr, "Size parse error: %s\n", sizespec);
	exit(-2);	
}

static struct fuse_lowlevel_ops big_ll_oper = {
	.lookup		= big_ll_lookup,
	.getattr	= big_ll_getattr,
	.readdir	= big_ll_readdir,
	.open		= big_ll_open,
	.read		= big_ll_read,
};


typedef struct {
	char *base, *block_size_str, *total_size_str;
} big_opts;

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_chan *ch;
	char *mountpoint;
	big_ctx ctx;
	int err = -1;
	
	struct fuse_opt optlist[] = {
		{ "--content %s", offsetof(big_opts, base), 0 },
		{ "-c %s", offsetof(big_opts, base), 0 },
		{ "--block-size %s", offsetof(big_opts, block_size_str), 0 },
		{ "-b %s", offsetof(big_opts, block_size_str), 0 },
		{ "--size %s", offsetof(big_opts, total_size_str), 0 },
		{ "-s %s", offsetof(big_opts, total_size_str), 0 },
	};
	big_opts opts = { NULL, NULL, NULL };	
	
	if (fuse_opt_parse(&args, &opts, optlist, NULL) == -1) {
		fprintf(stderr, "Bad opts\n");
		exit(-2);
	}
	
	ctx.block_size = parse_size(opts.block_size_str, "128K");
	ctx.total_size = parse_size(opts.total_size_str, "1T");
	
	// Initialize our block data
	if (!(ctx.basebuf = calloc(1, ctx.block_size))) {
		fprintf(stderr, "Out of mem\n");
		exit(-1);
	}
	if (opts.base) { // Given a filename, 
		int fd = open(opts.base, O_RDONLY);
		if (fd == -1) {
			fprintf(stderr, "Can't open base file\n");
			exit(-1);
		}
		if (read(fd, ctx.basebuf, ctx.block_size) <= 0) {
			fprintf(stderr, "Bad read\n");
			exit(-1);
		}
	}	
	
	if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
	    (ch = fuse_mount(mountpoint, &args)) != NULL) {
		struct fuse_session *se;

		se = fuse_lowlevel_new(&args, &big_ll_oper,
				       sizeof(big_ll_oper), &ctx);
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
