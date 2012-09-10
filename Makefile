CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall
CXXFLAGS=-Wall
LDFLAGS=-lfuse -lsndfile

fuse-convolve: fuse-convolve.o convolver.o conversion-buffer.o
	g++ $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

copy-music: copy-music.cc
	g++ $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm fuse-convolve *.o
