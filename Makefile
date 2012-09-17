CC=gcc
CXX=g++
F_VERSION=$(shell git log -n1 --date=short --format="%cd (id=%h)" 2>/dev/null || echo "[unknown version - compile from git]")
CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -O2 -DFOLVE_VERSION='"$(F_VERSION)"'
#CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -g -O0
CXXFLAGS=$(CFLAGS)
LDFLAGS=-lfuse -lsndfile -lzita-convolver -lmicrohttpd

OBJECTS = folve-main.o folve-filesystem.o conversion-buffer.o \
          file-handler-cache.o status-server.o util.o \
          zita-audiofile.o zita-config.o zita-fconfig.o zita-sstring.o 

folve: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f folve $(OBJECTS)
