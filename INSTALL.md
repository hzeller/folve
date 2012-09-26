## Typical installation ##

On a reasonably fresh system (e.g. Ubuntu 11.10 and 12.04), installation is
straightforward. To compile, this is what you need to do:

    sudo apt-get install libsndfile-dev libflac-dev libzita-convolver-dev \
                         libfuse-dev libboost-thread-dev libmicrohttpd-dev
    make

To install in the default location /usr/local/bin, just do

    sudo make install

.. otherwise specify the alternative location with DESTDIR

    sudo make DESTDIR=/usr install

## Older Systems ##
Older systems, e.g. Ubuntu 10.04 lack sufficiently, recent libraries for fuse
and the zita convolver. You've compile these yourself.

    # The FUSE library
    cd /tmp
    wget http://sourceforge.net/projects/fuse/files/fuse-2.X/2.9.1/fuse-2.9.1.tar.gz
    tar xvzf fuse-2.9.1.tar.gz
    cd fuse-2.9.1/
    ./configure
    make
    sudo make install

also

    # The Zita convolver
    cd /tmp
    wget http://kokkinizita.linuxaudio.org/linuxaudio/downloads/zita-convolver-3.1.0.tar.bz2
    tar xvjf zita-convolver-3.1.0.tar.bz2
    cd zita-convolver-3.1.0/libs
    make
    sudo make LIBDIR=lib install

Now, the compilation step in 'Typical installation' should succeed.
