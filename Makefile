CXX=g++
DESTDIR=/usr/local

F_VERSION=$(shell git log -n1 --date=short --format="%cd (commit=%h)" 2>/dev/null || echo "[unknown version - compile from git]")

CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -O2 -DFOLVE_VERSION='"$(F_VERSION)"'

CXXFLAGS=$(CFLAGS)
LDFLAGS=-lfuse -lsndfile -lzita-convolver -lmicrohttpd -lboost_thread-mt

OBJECTS = folve-main.o folve-filesystem.o conversion-buffer.o \
          sound-processor.o file-handler-cache.o status-server.o util.o \
          zita-audiofile.o zita-config.o zita-fconfig.o zita-sstring.o

folve: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

install: folve
	install folve $(DESTDIR)/bin

clean:
	rm -f folve $(OBJECTS)

html : README.html INSTALL.html

%.html : %.md
	markdown < $^ > $@
