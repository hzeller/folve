Configuration file format for Folve
-------------------------------------

The filters Folve should use are in configured in configuration directory that
you provide with the -C option.

Within that directory, there are directories with the names of the various
filters/effects you want to offer with folve. A naming scheme allows to match
sample-rates and bit-rates.
See https://github.com/hzeller/folve#filter-configuration
about naming of the files inside there. The demo-filters/ directory shows
some examples.

Looking at the commented example config files already gives you a good idea
how things work, but here is a more detailed description of the available
configuration options.

Folve uses the same configuration file format as jconvolver and fconvolver,
so the remaining README is a copy of the README.CONFIG in the
jconvolver project.

/convolver/new  <inputs> <outputs> <partition size> <maximum impulse length> <density>

    This command is always required and must be first one. The 'partition
    size' is the minumum partition size that will be used, and should be
    between 1 and 16 times the Jack period size. It will be adjusted (with
    a warning) otherwise. Processing delay will be zero if this is set to
    the Jack period size.

    The 'maximum impulse length' has little or no effect on CPU usage, but
    determines the amount of memory used, and will determine the sequence
    of partition sizes that will be used, so it should not be much larger
    than the longest convolution you want to use. Unused inputs or outputs
    do not take significant CPU or memory.

    The optional 'density' parameter should be between 0 and 1. It should
    be representative of the fraction of possible input/output pairs that
    will actually have a convolution defined between them. It is used as a
    hint to optimize the sequence of partition sizes for short and medium
    length convolutions.


/input/name <input number> <port name> {<source port>}
/output/name <output number> <port name> {<destination port>}

    These can be used to provide more informative ports names,
    and to optionally connect the inputs or outputs. Input and
    output numbers start at 1.


/cd <path>

    Change the directory where impulse response files are searched for to 'path'.
    Permits the use of short names in the next command. Initial value is the
    current directory.


/impulse/read   <input> <output> <gain> <delay> <offset> <length> <channel> <file>

    Read impulse from a sound file. 'Input', 'output' and 'channel'
    start from 1. Impulse files are read by libsndfile.
    'Gain' is the linear gain (i.e. not in dB) applied to the response.
    'Delay' sets the number of zero samples inserted before any data read.
    'Offset' can be used to skip frames at the start of the file.
    'Length' is the number of frames used, or zero for all.


/impulse/dirac  <input> <output> <gain> <delay>

    Create an impulse response consisting of a single sample of amplitude
    'gain' at position 'delay'. This is mainly used together with the
    /impulse/hilbert command to create complex matrices. Don't use this
    to measure CPU usage as only a single partition will be computed.


/impulse/hilbert  <input> <output> <gain> <delay> <length>

    Create an hilbert transform impulse response of the given lenght and
    having a delay equal to 'delay', which must be at least half the lenght
    (plus any latency compensation). The 90 degrees phase relation compared
    to a Dirac impulse with the same delay will be exact at all frequencies.
    The magnitude response wich will roll off at low frequencies, with a -3
    dB point at approximately the sample frequency divided by the length.


The commands /impulse/read, /impulse/dirac and /impulse/hilbert
can be used any number of times on the same input/output pair.
The individual impulse responses will be added.


/impulse/copy  <input> <output> <from input> <from output>

    Copy a convolution to another input/output pair. This can save a lot
    of memory if the same long convolution has to be performed on many
    in/out pairs. This is a 'symbolic link' - forward transformed impulse
    data will be shared for such copies. This includes any additions made
    to the original after the copy has been made.
