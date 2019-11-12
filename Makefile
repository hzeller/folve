CXX=g++
PREFIX=/usr/local

F_VERSION=$(shell git log -n1 --date=short --format="%cd (commit=%h)" 2>/dev/null || echo "[unknown version - compile from git]")

SNDFILE_INC?=$(shell pkg-config --cflags sndfile)
SNDFILE_LIB?=$(shell pkg-config --libs sndfile)

FUSE_INC?=$(shell pkg-config --cflags fuse3)
FUSE_LIB?=$(shell pkg-config --libs fuse3)

CXXFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -Wextra -W -Wno-unused-parameter -O3 -DFOLVE_VERSION='"$(F_VERSION)"' $(SNDFILE_INC) $(FUSE_INC)

LDFLAGS= -lzita-convolver -lmicrohttpd -lfftw3f $(FUSE_LIB) $(SNDFILE_LIB) -lpthread

ifdef LINK_STATIC
# static linking requires us to be much more explicit when linking
LDFLAGS+=-lFLAC -lvorbisenc -lvorbis -logg -lstdc++ -lm -lrt -ldl
LD_STATIC=-static
endif

OBJECTS = folve-main.o folve-filesystem.o conversion-buffer.o \
          processor-pool.o buffer-thread.o \
	  pass-through-handler.o convolve-file-handler.o \
          sound-processor.o file-handler-cache.o status-server.o util.o \
          zita-audiofile.o zita-config.o zita-fconfig.o zita-sstring.o

folve: $(OBJECTS)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LD_STATIC)

install: folve
	install folve $(PREFIX)/bin

clean:
	rm -f folve $(OBJECTS)

html : README.html INSTALL.html

%.html : %.md
	markdown < $^ > $@
