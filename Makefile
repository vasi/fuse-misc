OPT = -O0 -g

FUSE_CFLAGS = $(shell pkg-config --cflags fuse)
FUSE_LIBS = $(shell pkg-config --libs fuse)

PROGS = hello hello_ll many tree_write tree_ll dup_ll big_ll

all: $(PROGS)

clean:
	rm -f $(PROGS)

.PHONY: all clean

hello: hello.c
	$(CC) $(OPT) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

hello_ll: hello_ll.c
	$(CC) $(OPT) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

many: many.c
	$(CC) $(OPT) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

big_ll: big_ll.c
	$(CC) $(OPT) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

tree_write: tree_write.cc
	$(CXX) $(OPT) -o $@ $<

tree_ll: tree_ll.cc
	$(CXX) $(OPT) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)

dup_ll: dup_ll.cc
	$(CXX) $(OPT) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS)
