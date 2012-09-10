CC=gcc
CXX=g++
CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -g
CXXFLAGS=-Wall -g
LDFLAGS=-lfuse -lsndfile -lzita-convolver

fuse-convolve: fuse-convolve.o convolver.o conversion-buffer.o zita-audiofile.o zita-config.o zita-fconfig.o zita-sstring.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

copy-music: copy-music.cc
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm fuse-convolve *.o
