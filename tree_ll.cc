// (C) 2012 Dave Vasilevsky <dave@vasilevsky.ca>
// Licensing: GPL v2, see the COPYING file

#define FUSE_USE_VERSION 26

#include <fuse_lowlevel.h>

#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cerrno>
#include <cstring>

#include <string>
#include <vector>
#include <map>
using std::vector;
using std::string;
using std::map;

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(-1);
}

struct file {
	struct stat st;
	map<string, size_t> entries;
	
	file() { }
	file(struct stat s) : st(s) { }
	file(const file& f) : st(f.st), entries(f.entries) { }
};

struct dup_ll {
  dup_ll() : base(0), mountpoint(0) { }
  const char *base;
  const char *mountpoint;
  
  vector<file> files;
  
  void parse() {
	files.resize(2);
	
  	int fd = open(base, O_RDONLY);
	struct stat st;
	while (read(fd, &st, sizeof(st))) {
		file f(st);
		if (S_ISDIR(st.st_mode)) {
			unsigned short len;
			while (read(fd, &len, sizeof(len))) {
				if (len == 0)
					break;
				size_t ino;
				read(fd, &ino, sizeof(ino));
				vector<char> buf(len);
				read(fd, &buf[0], len);
				string name(&buf[0], len);
				f.entries[name] = ino;
			}
		}
		files.push_back(f);
	}
	
	files[1] = files.back();
	close(fd);
  }
};

static void dup_ll_getattr(fuse_req_t req, fuse_ino_t ino,
		struct fuse_file_info *fi) {
	dup_ll* dup = (dup_ll*)fuse_req_userdata(req);
	fuse_reply_attr(req, &dup->files[ino].st, DBL_MAX);
}

static void dup_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
	dup_ll* dup = (dup_ll*)fuse_req_userdata(req);
	map<string, size_t>& es = dup->files[parent].entries;
	map<string, size_t>::iterator iter = es.find(name);
	if (iter == es.end()) {
	    fuse_reply_err(req, ENOENT);
	    return;
	}
	
	fuse_entry_param e;
	size_t ino = iter->second;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = e.entry_timeout = DBL_MAX;
	e.attr = dup->files[ino].st;
    e.ino = ino;
	fuse_reply_entry(req, &e);
}

struct diriter {
	map<string, size_t>::iterator iter, end;
	diriter(map<string, size_t>& es) : iter(es.begin()), end(es.end()) { }
};

static void dup_ll_opendir(fuse_req_t req, fuse_ino_t ino,
		struct fuse_file_info *fi) {
	dup_ll* dup = (dup_ll*)fuse_req_userdata(req);
	file& f = dup->files[ino];
	if (!S_ISDIR(f.st.st_mode)) {
		fuse_reply_err(req, ENOTDIR);
		return;
	}
	
	fi->fh = (intptr_t)new diriter(f.entries);
	fuse_reply_open(req, fi);
}

static void dup_ll_releasedir(fuse_req_t req, fuse_ino_t ino,
		struct fuse_file_info *fi) {
	delete((diriter*)fi->fh);
	fuse_reply_err(req, 0);
}

static void dup_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
		off_t off, struct fuse_file_info *fi) {
	diriter *di = (diriter*)fi->fh;
	if (di->iter == di->end) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}
	 
	struct stat st;
	memset(&st, 0, sizeof(st));
	st.st_ino = di->iter->second;
	
	const char *name = di->iter->first.c_str();
	size_t sz = fuse_add_direntry(req, NULL, 0, name, NULL, 0);
	vector<char> buf(sz);
	fuse_add_direntry(req, &buf[0], sz, name, &st, off + sz);
	++di->iter;
	fuse_reply_buf(req, &buf[0], sz);
}

static void dup_ll_open(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  if ((fi->flags & 3) != O_RDONLY) {
    fuse_reply_err(req, EACCES);
    return;
  }
  fuse_reply_open(req, fi);
}

static void dup_ll_release(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  fuse_reply_err(req, 0);
}

static void dup_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
    off_t off, struct fuse_file_info *fi) {
  fuse_reply_buf(req, NULL, 0);
}

static int dup_ll_opt_proc(void *data, const char *arg, int key,
		struct fuse_args *outargs) {
	dup_ll *dup = (dup_ll*)data;
	if (key == FUSE_OPT_KEY_NONOPT) {
		if (dup->mountpoint) {
			return -1; // Too many args
		} else if (dup->base) {
      dup->mountpoint = arg;
			return 1;
		} else {
			dup->base = realpath(arg, NULL);
			return 0;
		}
	}
	return 1; // Keep
}

int main(int argc, char *argv[]) {
	struct fuse_lowlevel_ops ops;
	memset(&ops, 0, sizeof(ops));
	ops.lookup		= dup_ll_lookup;
	ops.getattr	= dup_ll_getattr;
	ops.opendir	= dup_ll_opendir;
	ops.readdir	= dup_ll_readdir;
	ops.releasedir	= dup_ll_releasedir;
	ops.open	    = dup_ll_open;
	ops.read	    = dup_ll_read;
	ops.release  = dup_ll_release;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_chan *ch;
	char *mountpoint;
	int err = -1;

	dup_ll ll;
  if (fuse_opt_parse(&args, &ll, NULL, dup_ll_opt_proc) == -1)
    die("bad opts");
  ll.parse();
  
  if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
	    (ch = fuse_mount(mountpoint, &args)) != NULL) {
		struct fuse_session *se;

    se = fuse_lowlevel_new(&args, &ops, sizeof(ops), &ll);
		if (se != NULL) {
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
