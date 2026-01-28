CC=g++

MONGODB_CFLAGS := $(shell pkg-config --cflags libmongoc-1.0)

CFLAGS=-std=c++11 -c -g -Wall -fgnu-tm
CFLAGS += $(MONGODB_CFLAGS)
INCLUDES=-I../
SOURCES=$(wildcard *.cc)
OBJECTS=$(SOURCES:.cc=.o)

all: $(SOURCES) $(OBJECTS)

.cc.o:
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@

clean:
	$(RM) $(OBJECTS)

