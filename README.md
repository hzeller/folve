Folve - Fuse Convolve
=====================
A fuse filesystem for on-the-fly convolving of audio files.

Overview
--------

This fuse filesystem takes an original path to a directory with flac-files
and provides these files at the mount point. Accessing audio files will
automatically convolve these on-the-fly using the FIR zita convolver by
Fons Adriaensen. You can directly use filter configuration files that you have
for jconvolver/fconvolver (files in this directory starting with zita-* are
imported from his jconvolver project to parse the same configuration files).
These config files need have a special naming scheme, see below.

Folve solves the problem that many media servers don't provide a convolving
option and their only interface to the outside world is to access a file
system. So here we provide a filesystem, convolving files while they read them :)
In general the beauty of simply accessing audio files that are transparently
convolved is very useful and powerful in other contexts too.

Filesystem accesses are optimized for streaming. If files are read sequentially,
we only need to convolve whatever is requested, which minimizes CPU use if
you don't need the full file. Simply playing a file in real-time will use very
little CPU (on my notebook ~3% on one core). So this should work as well on
low-CPU machines (like NAS servers; haven't tried that yet).

Because input and output files are compressed, we can't predict what the
relationship between file-offset and sample-number is; so skipping forward
requires to convolve everything up to the point (the zita convolver is
pretty fast though, so you'll hardly notice).

While indexing, some media servers try to skip to the end of the file (don't
know why, to check if the end is there ?), so there is code that detects this
case so that we don't end up convolving whole files just for this. Also, some
media servers continually watch the file size while playing, so we adapt
predictions of the final filesize depending on the observed compression ratio.

The files are decoded with libsndfile, convolved, and re-encoded with
libsndfile. Libsndfile is very flexible in reading/writing all kinds
of audio files, but the support for rich header tags is limited. To not loose
information from the flac-headers when indexing Folve-served files with a
media server, Folve extracts and serves the headers from the original files
before continuing with the convolved audio stream.

Folve has been tested with some players and media servers (and
works around bugs in these). Still, this is the first version made public, so
expect rough edges. Please report observations with particular media servers
or send patches to <h.zeller@acm.org>.

This project is notably based on

 * Fuse: Filesystem in Userspace   <http://fuse.sourceforge.net/>
 * JConvolver audio convolver <http://apps.linuxaudio.org/apps/all/jconvolver>
 * LibSndfile r/w audio files <http://www.mega-nerd.com/libsndfile/>
 * Microhttpd webserver library <http://www.gnu.org/software/libmicrohttpd/>


### Compiling on Ubuntu (tested on 11.10 and 12.04) ###

  This requires the latest versions of libfuse and libzita convolver to compile.
  .. and a couple of other libs:

    $ sudo aptitude install libsndfile-dev libflac-dev libzita-convolver-dev \
                            libfuse-dev libboost-dev libmicrohttpd-dev
    $ make
    $ sudo make install

(TODO: debian package)

### Run ###
Folve requires at least two parameters: the directory where your original
*.flac files reside and the mount point of this filesystem.

Also, do be useful, you need to supply at least one configuration directory
with the -c <config-dir> option. Very useful is the -p <port> that starts
an HTTP status server. Let's use some example filters from this distribution;
if you are in the folve source directory, you find the directory `demo-filters/`
that contains subdirectories with filters.
Let's choose the lowpass and highpass filter to play with:

    $ mkdir /tmp/test-mount
    $ ./folve -c demo-filters/lowpass -c demo-filters/highpass -p 17322 -f \
         /path/to/your/directory/with/flacs /tmp/test-mount

Now you can access the fileystem under that mount point; it has the same
structure as under your `/path/to/your/directory/with/flacs`

    $ mplayer /tmp/test-mount/foo.flac

Folve provides a HTTP status page; have a look at

    http://localhost:17322/

(or whatever port you chose)
There you can switch the filter - change it and re-open the same flac file:
you'll hear the difference.

To finish this instance of folve, you can just press CTRL-C as we've run it in
the foreground (the `-f` option did this). In real life, you'd run it as daemon
(without `-f` option), so then you can unmount with the `fusermount` command:

    $ fusermount -u /tmp/test-mount

### Filter Configuration ###
The configuration directory should contain configuration files as they're
found in jconvolver, with the following naming scheme:

     filter-<samplerate>-<channels>-<bits>.conf   OR
     filter-<samplerate>-<channels>.conf          OR
     filter-<samplerate>.conf

So if you have flac files with 44.1kHz, 16 bits and 2 channel stero,
you need a filter configuration named one of these (in matching sequence):

     /filter/dir/filter-44100-2-16.conf            OR
     /filter/dir/filter-44100-2.conf               OR
     /filter/dir/filter-44100.conf

The files are searched from the most specific to the least specific type.

(See README.CONFIG in the [jconvolver](http://apps.linuxaudio.org/apps/all/jconvolver) project how these look like)

### General usage: ###

    usage: folve [options] <original-dir> <mount-point>
    Options: (in sequence of usefulness)
      -c <cfg-dir> : Convolver configuration directory.
                     You can supply this option multiple times:
                     Select with radio-buttons on the HTTP status page.
      -p <port>    : Port to run the HTTP status server on.
      -r <refresh> : Seconds between refresh of status page;
                     Default is 10 seconds; switch off with -1.
      -g           : Gapless convolving alphabetically adjacent files.
      -D           : Moderate volume Folve debug messages to syslog.
                     Can then also be toggled in the UI.
      -f           : Operate in foreground; useful for debugging.
      -o <mnt-opt> : other generic mount parameters passed to fuse.
      -d           : High volume fuse debug log. Implies -f.

The Folve filesystem will determine the samplerate/bits/channels and
attempt to find the right filter in the filter directory. If there is a filter,
the output is filtered on-the-fly, otherwise the original file is returned.

### Misc ###
To manually switch the configuration from the command line, you can use `wget`
or `curl`, whatever you prefer:

    wget -q -O/dev/null http://localhost:17322/settings?f=2
    curl http://localhost:17322/settings?f=2

The parameter given to `f=` is the configuration in the same sequence you
supplied on startup, starting to count from 1. Configuration 0 means
'no filter' (And no, there is no security built-in. If you want people from
messing with the configuration of your Folve-daemon, don't use -p :)).
