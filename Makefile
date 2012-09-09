CFLAGS=-D_FILE_OFFSET_BITS=64 -Wall
LDFLAGS=-lfuse

fuse-convolve: fuse-convolve.c
	gcc $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm fuse-convolve
