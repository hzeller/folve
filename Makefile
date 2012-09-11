CC=gcc
CXX=g++
CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -g -O2
CXXFLAGS=-Wall -g -O2
LDFLAGS=-lfuse -lsndfile -lzita-convolver

fuse-convolve: fuse-convolve.o convolver.o conversion-buffer.o zita-audiofile.o zita-config.o zita-fconfig.o zita-sstring.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

copy-music: copy-music.cc
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f fuse-convolve *.o
