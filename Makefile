CC=g++

MONGOC_CFLAGS=$(pkg-config --cflags libmongoc-1.0)
MONGOC_LIBS=$(pkg-config --libs libmongoc-1.0)

CFLAGS=-std=c++11 -g -Wall -pthread -I./
LDFLAGS= -lpthread -ltbb -lhiredis

CFLAGS += $(MONGOC_CFLAGS)
LDFLAGS += $(MONGOC_LIBS)

SUBDIRS=core db redis
SUBSRCS=$(wildcard core/*.cc) $(wildcard db/*.cc)
OBJECTS=$(SUBSRCS:.cc=.o)
EXEC=ycsbc

all: $(SUBDIRS) $(EXEC)

$(SUBDIRS):
	$(MAKE) -C $@

$(EXEC): $(wildcard *.cc) $(OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir $@; \
	done
	$(RM) $(EXEC)

.PHONY: $(SUBDIRS) $(EXEC)

