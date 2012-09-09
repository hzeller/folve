CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall
CXXFLAGS=-Wall
LDFLAGS=-lfuse

fuse-convolve: fuse-convolve.o convolver.o
	g++ $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm fuse-convolve *.o
