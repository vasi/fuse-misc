// (C) 2012 Dave Vasilevsky <dave@vasilevsky.ca>
// Licensing: GPL v2, see the COPYING file

#include <vector>
#include <string>
using std::vector;
using std::string;

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

int fd;
size_t ino = 2;

struct entry {
	size_t ino;
	string name;
	entry(size_t i, string n) : ino(i), name(n) { }
	
	void write_entry(int fd) {
		unsigned short len = name.size();
		write(fd, &len, sizeof(len));
		write(fd, &ino, sizeof(ino));
		write(fd, name.c_str(), len);
	}
};

size_t write_path(string &dir, char *sub) {
	string path(dir);
	if (sub)
		path = path + "/" + sub;
	struct stat st;
	lstat(path.c_str(), &st);
	
	vector<entry> entries;
	bool is_dir = S_ISDIR(st.st_mode);
	if (is_dir) {
		DIR *d = opendir(path.c_str());
		if (d) {
			struct dirent *de;
			while ((de = readdir(d))) {
				if (string(".") == de->d_name || string("..") == de->d_name)
					continue;
				entries.push_back(
					entry(write_path(path, de->d_name), de->d_name));
			}
			closedir(d);
		}
	}
	
	st.st_dev = 0;
	st.st_ino = ino++;
	write(fd, &st, sizeof(st));
	if (is_dir) {	
		vector<entry>::iterator iter(entries.begin());
		for (; iter != entries.end(); ++iter)
			iter->write_entry(fd);
		unsigned short eoe = 0;
		write(fd, &eoe, sizeof(eoe));
	}
	return st.st_ino;
}

int main(int argc, char *argv[]) {
	string dir(argv[1]);
	fd = open(argv[2], O_WRONLY | O_CREAT, 0777);
	write_path(dir, NULL);
	
	return 0;
}
