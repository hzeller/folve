CC=gcc
CXX=g++
CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -O2
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
