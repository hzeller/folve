// -------------------------------------------------------------------------
//
//    Copyright (C) 2009 Fons Adriaensen <fons@linuxaudio.org>
//    
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// -------------------------------------------------------------------------


#include <stdlib.h>
#include <string.h>
#include "zita-audiofile.h"


Audiofile::Audiofile (void)
{
    reset ();
}


Audiofile::~Audiofile (void)
{
    close ();
}


void Audiofile::reset (void)
{
    _sndfile = 0;
    _mode = MODE_NONE;
    _type = TYPE_OTHER;
    _form = FORM_OTHER;
    _rate = 0;
    _chan = 0;
    _size = 0;
}


int Audiofile::open_read (const char *name)
{
    SF_INFO I;

    if (_mode) return ERR_MODE;
    reset ();

    if ((_sndfile = sf_open (name, SFM_READ, &I)) == 0) return ERR_OPEN;

    _mode = MODE_READ;

    switch (I.format & SF_FORMAT_TYPEMASK)
    {
    case SF_FORMAT_CAF:
	_type = TYPE_CAF;
	break;
    case SF_FORMAT_WAV:
	_type = TYPE_WAV;
	break;
    case SF_FORMAT_WAVEX:
        if (sf_command (_sndfile, SFC_WAVEX_GET_AMBISONIC, 0, 0) == SF_AMBISONIC_B_FORMAT)
	    _type = TYPE_AMB;
	else
            _type = TYPE_WAV;
    }

    switch (I.format & SF_FORMAT_SUBMASK)
    {
    case SF_FORMAT_PCM_16:
	_form = FORM_16BIT;
	break;
    case SF_FORMAT_PCM_24:
	_form = FORM_24BIT;
	break;
    case SF_FORMAT_PCM_32:
	_form = FORM_32BIT;
	break;
    case SF_FORMAT_FLOAT:
	_form = FORM_FLOAT;
	break;
    }

    _rate = I.samplerate;
    _chan = I.channels;
    _size = I.frames;

    return 0;
}


int Audiofile::open_write (const char *name, int type, int form, int rate, int chan)    
{
    SF_INFO I;

    if (_mode) return ERR_MODE;
    if (!rate || !chan) return ERR_OPEN;
    reset ();

    switch (type)
    {
    case TYPE_CAF:
	I.format = SF_FORMAT_CAF;
	break;
    case TYPE_WAV:
    case TYPE_AMB:
        I.format = (chan > 2) ? SF_FORMAT_WAVEX : SF_FORMAT_WAV;
	break;
    default:
        return ERR_TYPE;
    }

    switch (form)
    {
    case FORM_16BIT:
	I.format |= SF_FORMAT_PCM_16;
	break;
    case FORM_24BIT:
	I.format |= SF_FORMAT_PCM_24;
	break;
    case FORM_32BIT:
	I.format |= SF_FORMAT_PCM_32;
	break;
    case FORM_FLOAT:
	I.format |= SF_FORMAT_FLOAT;
	break;
    default:
        return ERR_FORM;
    }

    I.samplerate = rate;
    I.channels = chan;
    I.sections = 1;

    if ((_sndfile = sf_open (name, SFM_WRITE, &I)) == 0) return ERR_OPEN;

    if (type == TYPE_AMB)
    {
        sf_command (_sndfile, SFC_WAVEX_SET_AMBISONIC, 0, SF_AMBISONIC_B_FORMAT);
    }

    _mode = MODE_WRITE;
    _type = type;
    _form = form;
    _rate = rate;
    _chan = chan;

    return 0;
}


int Audiofile::close (void)
{
    if (_sndfile) sf_close (_sndfile);
    reset ();
    return 0;
}


int Audiofile::seek (uint32_t posit)
{
    if (!_sndfile) return ERR_MODE;
    if (sf_seek (_sndfile, posit, SEEK_SET) != posit) return ERR_SEEK;
    return 0;
}


int Audiofile::read (float *data, uint32_t frames)
{
    if (_mode != MODE_READ) return ERR_MODE;
    return sf_readf_float (_sndfile, data, frames);
}


int Audiofile::write (float *data, uint32_t frames)
{
    if (_mode != MODE_WRITE) return ERR_MODE;
    return sf_writef_float (_sndfile, data, frames);
}


