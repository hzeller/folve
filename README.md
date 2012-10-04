Folve - FUSE convolve
=====================
Folve is a FUSE filesystem that convolves audio files on-the-fly.

Overview
--------

The Folve FUSE filesystem takes a path to a directory of FLAC files, and provides
these files at a mount point. Other file formats than FLAC should work as well,
but not all are working well for streaming yet (and before you ask: MP3 is not
supported. Use Ogg/Vorbis as it is patent free and provides better quality
than MP3).

When a FLAC file is accessed through the mount point, Folve automatically
convolves its original counterpart on-the-fly with a Finite Impulse
Response (FIR) filter. The FIR filter is based on the jconvolver convolution
engine.

Folve can use the same filter configuration files that jconvolver uses. Folve
requires a naming scheme, described later, for these files.

In essence, Folve provides a filesystem that convolves files as a media server
or application reads them; many media servers or applications do not provide
an independent convolve option, but they all can read files.

Filesystem accesses are optimized for streaming. If files are read sequentially,
we only need to convolve whatever is requested, which minimizes CPU use if
you do not need the full file. Simply playing a file in real-time will use very
little CPU (on my notebook ~3% on one core). So this should work as well on
low-CPU machines (like NAS servers; have not tried that yet).

Because input and output files are compressed, we cannot predict what the
relationship between file-offset and sample-number is; so skipping forward
requires to convolve everything up to the point (the convolver is pretty fast
though, so you'll hardly notice).

While indexing, some media servers try to skip to the end of the file (do not
know why, to check if the end is there ?), so there is code that detects this
case so that we do not end up convolving whole files just for this. Also, some
media servers continually watch the file size while playing, so we adapt
predictions of the final filesize depending on the observed compression ratio.

The files are decoded with libsndfile, convolved, and re-encoded with
libsndfile. Libsndfile is very flexible in reading/writing all kinds
of audio files, but the support for rich header tags is limited. To not loose
information from the FLAC headers when indexing Folve-served files with a
media server, Folve extracts and serves the headers from the original files
before continuing with the convolved audio stream.

Folve has been tested with some players and media servers (and
works around bugs in these). Still, this is the first version made public, so
expect rough edges. Please report observations with particular media servers
or provide patches through github
<https://github.com/hzeller/folve>.

This project is notably based on

 * FUSE: Filesystem in Userspace   <http://fuse.sourceforge.net/>
 * Zita Convolver <http://kokkinizita.linuxaudio.org/linuxaudio/downloads/zita-convolver-3.1.0.tar.bz2>
 * JConvolver <http://apps.linuxaudio.org/apps/all/jconvolver>
     * Program files in the Folve project named zita-*.{h,cc} are derivatives of
       files found in the jconvolver project. They implement the compatible
       configuration file parsing.
 * LibSndfile r/w audio files <http://www.mega-nerd.com/libsndfile/>
 * Microhttpd webserver library <http://www.gnu.org/software/libmicrohttpd/>


### Compiling on Ubuntu (tested on 11.10 and 12.04) ###

  This requires the latest versions of some development libraries.

    sudo apt-get install libsndfile-dev libflac-dev libzita-convolver-dev \
                         libfuse-dev libmicrohttpd-dev
    make
    sudo make install

For hints on how to compile on older systems see INSTALL.md.

(TODO: debian package)

### Let's test it! ###
Folve requires at least two parameters: the directory where your original
FLAC files reside and the mount point of this filesystem.

Also, do be useful, you need to supply the directory that contains filter
directories with the `-C <config-dir>` option.
Very useful is the `-p <port>` that starts a HTTP status server. Let's use
some example filters from this distribution;
if you are in the Folve source directory, you find the directory `demo-filters/`
that contains subdirectories with filters.

    mkdir /tmp/test-mount
    ./folve -C demo-filters -p 17322 -f \
            /path/to/your/directory/with/flacs /tmp/test-mount

Now you can access the fileystem under that mount point; it has the same
structure as your original directory.

    mplayer /tmp/test-mount/foo.flac

Folve provides a HTTP status page; have a look at

    http://localhost:17322/

(or whatever port you chose with the `-p 17322` option)
There you can switch the filter; after you changed it in the UI, re-open
the same FLAC file with your media player: you'll hear the difference.

To terminate this instance of folve, you can just press CTRL-C as we've run it
in the foreground (the `-f` option did this). In real life, you'd run it as
daemon (without `-f` option), so then you can stop the daemon and unmount the
directory with the `fusermount` command:

    fusermount -u /tmp/test-mount

### Filter Configuration ###
Filters are WAV files containing an impulse response (IR). This is
used by jconvolver's convolution engine to create a
[Finite Impulse Response](http://en.wikipedia.org/wiki/Finite_impulse_response)
(FIR) filter and process your audio.

Text configuration files refer to these WAV files and add parameters such as
filter gain and channel mapping. These configuration files are read by Folve.
See the samples in the `demo-filters/` directory. The README.CONFIG in the
[jconvolver](http://apps.linuxaudio.org/apps/all/jconvolver)
project describes the details of the configuration format.

Since the filter is dependent on the sampling rate, we need to choose the right
filter depending on the input file we see. This is why you give Folve a whole
configuration directory: it can contain multipe files depending on sample rate.

The files in the configuration directory need to follow a naming scheme to
be found by Folve. Their naming is:

    filter-<samplerate>-<channels>-<bits>.conf   OR
    filter-<samplerate>-<channels>.conf          OR
    filter-<samplerate>.conf

So if you have FLAC files with 44.1kHz, 16 bits and 2 channel stero,
you need a filter configuration named one of these (in matching sequence):

    /filter/dir/filter-44100-2-16.conf            OR
    /filter/dir/filter-44100-2.conf               OR
    /filter/dir/filter-44100.conf

The files are searched from the most specific to the least specific type.

The Folve filesystem will determine the samplerate/bits/channels and
attempt to find the right filter in the filter directory. If there is a filter,
the output is filtered on-the-fly, otherwise the original file is returned.

(I am looking for filter construction tools on Linux; if you know some,
please let me know.)

### General usage: ###

    usage: folve [options] <original-dir> <mount-point-dir>
    Options: (in sequence of usefulness)
      -c <cfg-dir> : Convolver configuration directory.
                     You can supply this option multiple times:
                     Select on the HTTP status page.
      -p <port>    : Port to run the HTTP status server on.
      -r <refresh> : Seconds between refresh of status page;
                     Default is 10 seconds; switch off with -1.
      -g           : Gapless convolving alphabetically adjacent files.
      -D           : Moderate volume Folve debug messages to syslog.
      -f           : Operate in foreground; useful for debugging.
      -o <mnt-opt> : other generic mount parameters passed to FUSE.
      -d           : High volume FUSE debug log. Implies -f.

If you're listening to classical music, opera or live-recordings, then you
certainly want to switch on gapless convolving with `-g`. If a file ends with
not enough samples to fill the FIR filter input, the gap is bridged by
including the first samples of the alphabetically next file in that
directory -- and the result is split between these two files.

### Misc ###
To manually switch the configuration from the command line, you can use `wget`
or `curl`, whatever you prefer:

    wget -q -O/dev/null http://localhost:17322/settings?f=2
    curl http://localhost:17322/settings?f=2

The parameter given to `f=` is the configuration in the same sequence you
supplied on startup, starting to count from 1. Configuration 0 means
'no filter' (And no, there is no security built-in. If you want people from
messing with the configuration of your Folve-daemon, do not use `-p <port>` :)).
