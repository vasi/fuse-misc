// (C) 2012 Dave Vasilevsky <dave@vasilevsky.ca>
// Licensing: GPL v2, see the COPYING file

#define FUSE_USE_VERSION 26

#include <fuse_lowlevel.h>

#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cerrno>
#include <cassert>
#include <cstring>

#include <dirent.h>
#include <unistd.h>

#include <string>
#include <map>
#include <vector>
using std::vector;
using std::string;
using std::map;


static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(-1);
}


struct dup_ll {
  dup_ll() : base(0), mountpoint(0) { }
  const char *base;
  const char *mountpoint;
  
  typedef map<fuse_ino_t, string> ino_map;
  ino_map inodes;
  
  string locate(fuse_ino_t ino) {
    if (ino == FUSE_ROOT_ID)
      return base;
    ino_map::const_iterator iter = inodes.find(ino);
    if (iter == inodes.end())
      return NULL;
    return iter->second;
  }
};

static string locate(fuse_req_t req, fuse_ino_t ino) {
  dup_ll* dup = (dup_ll*)fuse_req_userdata(req);
  return dup->locate(ino);
}

static int dup_stat(string& p, struct stat *st) {
  int r = lstat(p.c_str(), st);
  if (r != 0)
    return r;
  st->st_dev = 0;
  return r;
}

static void dup_ll_getattr(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  string p = locate(req, ino);
  struct stat st;
  if (dup_stat(p, &st) != 0) {
    fuse_reply_err(req, errno);
    return;
  }
  st.st_ino = ino;
  fuse_reply_attr(req, &st, DBL_MAX);
}

static void dup_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  string p = locate(req, parent);
  string c = p + "/" + name;
  
  struct fuse_entry_param e;
  memset(&e, 0, sizeof(e));
  e.attr_timeout = e.entry_timeout = DBL_MAX;
  if (dup_stat(c, &e.attr) != 0) {
    fuse_reply_err(req, errno);
    return;
  }
  e.ino = e.attr.st_ino;
  
  dup_ll* dup = (dup_ll*)fuse_req_userdata(req);
  dup->inodes[e.ino] = c;
  fuse_reply_entry(req, &e);
}

static void dup_ll_opendir(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  string p = locate(req, ino);
  DIR *d = opendir(p.c_str());
  if (!d) {
    fuse_reply_err(req, errno);
    return;
  }
  fi->fh = (intptr_t)d;
  fuse_reply_open(req, fi);
}

static void dup_ll_releasedir(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  closedir((DIR*)fi->fh);
  fuse_reply_err(req, 0);
}

static void dup_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
    off_t off, struct fuse_file_info *fi) {
  DIR *d = (DIR*)fi->fh;
  struct dirent *de = readdir(d);
  if (!de) {
    fuse_reply_buf(req, NULL, 0);
    return;
  }
  
  struct stat st;
  memset(&st, 0, sizeof(st));
  st.st_ino = de->d_ino;
  
  size_t sz = fuse_add_direntry(req, NULL, 0, de->d_name, NULL, 0);
  vector<char> buf(sz);
  fuse_add_direntry(req, &buf[0], sz, de->d_name, &st, off + sz);
  fuse_reply_buf(req, &buf[0], sz);
}

static void dup_ll_open(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  if ((fi->flags & 3) != O_RDONLY) {
    fuse_reply_err(req, EACCES);
    return;
  }
  
  string p = locate(req, ino);
  if ((fi->fh = open(p.c_str(), fi->flags)) == -1) {
    fuse_reply_err(req, errno);
    return;
  }
  fuse_reply_open(req, fi);
}

static void dup_ll_release(fuse_req_t req, fuse_ino_t ino,
    struct fuse_file_info *fi) {
  close(fi->fh);
  fuse_reply_err(req, 0);
}

static void dup_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
    off_t off, struct fuse_file_info *fi) {
  vector<char> buf(size);
  ssize_t r = pread(fi->fh, &buf[0], size, off);
  if (r == -1) {
    fuse_reply_err(req, errno);
    return;
  }
  fuse_reply_buf(req, &buf[0], r);
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
