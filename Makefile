CC=gcc
CXX=g++
CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall -g -O3
CXXFLAGS=-Wall -g -O3
LDFLAGS=-lfuse -lsndfile -ffast-math -lfftw3f -lpthread

fuse-convolve: fuse-convolve.o convolver.o conversion-buffer.o zita-audiofile.o zita-config.o zita-fconfig.o zita-sstring.o zita-convolver.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

copy-music: copy-music.cc
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f fuse-convolve *.o
