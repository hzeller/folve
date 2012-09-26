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
Older systems, e.g. Ubuntu 10.04 lack sufficiently recent libraries for FUSE
and the zita convolver. In that case, don't install these; better remove them
to avoid confusion:

    sudo apt-get remove libzita-convolver-dev libfuse-dev

.. and compile the latest versions yourself:

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

Now, the compilation step described in *Typical installation* should succeed.

## Linking statically ##
To install Folve on some embedded systems, you might need to link it statically.
This depends a lot on your system, so it might require some twiddeling, but
let's see the basic steps.

First, you need go create a static library of the zita-convolver. The default
installation does not provide this, so let's do it.
First, start with compiling the zita-convolver yourself, as described in the
*Older Systems* section.

Then run the following commands:

     # .. first run the zita compile commands from above. Then:
     cd /tmp/zita-convolver-3.1.0/libs
     ar rcs libzita-convolver.a zita-convolver.o
     ranlib libzita-convolver.a
     sudo install -m 644 libzita-convolver.a /usr/local/lib

Now we can compile folve statically; go into the folve source directory and
type

     make LINK_STATIC=y

The last step might fail if there are additional dependencies. Add them in the
`ifdef LINK_STATIC` section of the Makefile.
