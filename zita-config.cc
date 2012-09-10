//  -----------------------------------------------------------------------------
//
//  Copyright (C) 2006-2011 Fons Adriaensen <fons@linuxaudio.org>
//  
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
//  -----------------------------------------------------------------------------


#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <libgen.h>
#include "zita-audiofile.h"
#include "zita-config.h"


// zita-config
#define BSIZE  0x4000


static int check_inout (unsigned int ip, unsigned int op)
{
    if (! size) return ERR_NOCONV;
    if ((ip < 1) || (ip > ninp)) return ERR_IONUM;
    if ((op < 1) || (op > nout)) return ERR_IONUM;
    return 0;
}


static int readfile (const char *line, int lnum, const char *cdir)
{
    unsigned int  ip1, op1, k;
    float         gain;
    unsigned int  delay;
    unsigned int  offset;
    unsigned int  length;
    unsigned int  ichan, nchan; 
    int           n, ifram, nfram, err;
    char          file [1024];
    char          path [1024];
    Audiofile     audio;
    float         *buff, *p;

    if (sscanf (line, "%u %u %f %u %u %u %u %n",
                &ip1, &op1, &gain, &delay, &offset, &length, &ichan, &n) != 7) return ERR_PARAM;
    n = sstring (line + n, file, 1024);
    if (!n) return ERR_PARAM;

    k = latency;
    if (k)
    {
	if (delay >= k)
	{
	    delay -= k;
	}
	else 
	{
	    k -= delay;
	    delay = 0;
	    offset += k;
	    fprintf (stderr, "Line %d: First %d frames removed by latency compensation.\n", lnum, k);
	}
    }		
    err = check_inout (ip1, op1);
    if (err) return err;

    if (*file == '/') strcpy (path, file);
    else
    {
        strcpy (path, cdir);
        strcat (path, "/");
        strcat (path, file);
    }

    if (audio.open_read (path))
    {
        fprintf (stderr, "Line %d: Unable to open '%s'.\n", lnum, path);
        return ERR_OTHER;
    } 

    if (audio.rate () != (int) fsamp)
    {
        fprintf (stderr, "Line %d: Sample rate (%d) of '%s' does not match.\n",
                 lnum, audio.rate (), path);
    }

    nchan = audio.chan ();
    nfram = audio.size ();
    if ((ichan < 1) || (ichan > nchan))
    {
	fprintf (stderr, "Line %d: Channel not available.\n", lnum);
        audio.close ();
        return ERR_OTHER;
    } 
    if (offset && audio.seek (offset))
    {
	fprintf (stderr, "Line %d: Can't seek to offset.\n", lnum);
        audio.close ();
        return ERR_OTHER;
    } 
    if (! length) length = nfram - offset;
    if (length > size - delay) 
    {
	length = size - delay;
   	fprintf (stderr, "Line %d: Data truncated.\n", lnum);
    }

    try 
    {
        buff = new float [BSIZE * nchan];
    }
    catch (...)
    {
	audio.close ();
        return ERR_ALLOC;
    }

    while (length)
    {
	nfram = (length > BSIZE) ? BSIZE : length;
	nfram = audio.read (buff, nfram);
	if (nfram < 0)
	{
	    fprintf (stderr, "Line %d: Error reading file.\n", lnum);
	    audio.close ();
	    delete[] buff;
	    return ERR_OTHER;
	}
	if (nfram)
	{
	    p = buff + ichan - 1;
	    for (ifram = 0; ifram < nfram; ifram++) p [ifram * nchan] *= gain;
            if (convproc->impdata_create (ip1 - 1, op1 - 1, nchan, p, delay, delay + nfram))
            {
	        audio.close ();
                delete[] buff;
	        return ERR_ALLOC;
            }
	    delay  += nfram;
	    length -= nfram;
	}
    }

    audio.close ();
    delete[] buff;
    return 0;
}


static int impdirac (const char *line, int lnum)
{
    unsigned int  ip1, op1, k;
    unsigned int  delay;
    float         gain;
    int           stat;

    if (sscanf (line, "%u %u %f %u", &ip1, &op1, &gain, &delay) != 4) return ERR_PARAM;

    stat = check_inout (ip1, op1);
    if (stat) return stat;

    k = latency;
    if (delay < k)
    {
	fprintf (stderr, "Line %d: Dirac pulse removed: delay < latency.\n", lnum);
	return 0;
    }
    delay -= k;

    if (delay < size)
    {
	if (convproc->impdata_create (ip1 - 1, op1 - 1, 1, &gain, delay, delay + 1))
	{
	    return ERR_ALLOC;
	}
    }
    return 0;
}


static int imphilbert (const char *line, int lnum)
{
    unsigned int  ip1, op1;
    unsigned int  delay;
    unsigned int  length;
    unsigned int  i, h, k;
    float         gain, v, w;
    float         *hdata;
    int           stat;

    if (sscanf (line, "%u %u %f %u %u", &ip1, &op1, &gain, &delay, &length) != 5) return ERR_PARAM;

    stat = check_inout (ip1, op1);
    if (stat) return stat;

    if ((length < 64) || (length > 65536))
    {
	return ERR_PARAM;
    }
    k = latency;
    if (delay < k + length / 2)
    {
	fprintf (stderr, "Line %d: Hilbert impulse removed: delay < latency + lenght / 2.\n", lnum);
	return 0;
    }
    delay -= k + length / 2;
    hdata = new float [length];
    memset (hdata, 0, length * sizeof (float));

    gain *= 2 / M_PI;
    h = length / 2;
    for (i = 1; i < h; i += 2)
    {
	v = gain / i;
        w = 0.43f + 0.57f * cosf (i * M_PI / h);
	v *= w;
	hdata [h + i] = -v;
	hdata [h - i] =  v;
    }

    if (convproc->impdata_create (ip1 - 1, op1 - 1, 1, hdata, delay, delay + length))
    {
        return ERR_ALLOC;
    }

    delete[] hdata;
    return 0;
}


static int impcopy (const char *line, int lnum)
{
    unsigned int ip1, op1, ip2, op2;
    int          stat;

    if (sscanf (line, "%u %u %u %u", &ip1, &op1, &ip2, &op2) != 4) return ERR_PARAM;

    stat = check_inout (ip1, op1) | check_inout (ip2, op2);
    if (stat) return stat;

    if ((ip1 != ip2) || (op1 != op2))
    {
        if (convproc->impdata_copy (ip2 - 1, op2 - 1, ip1 - 1, op1 - 1)) return ERR_ALLOC;
    }
    else return ERR_PARAM;

    return 0;
}


int config (const char *config)
{
    FILE          *F;
    int           stat, lnum;
    char          line [1024];
    char          cdir [1024];
    char          *p, *q;

    if (! (F = fopen (config, "r"))) 
    {
	fprintf (stderr, "Can't open '%s' for reading\n", config);
        return -1;
    } 

    strcpy (cdir, dirname ((char *) config));
    stat = 0;
    lnum = 0;

    while (! stat && fgets (line, 1024, F))
    {
        lnum++;
        p = line; 
        if (*p != '/')
	{
            while (isspace (*p)) p++;
            if ((*p > ' ') && (*p != '#'))
	    {
                stat = ERR_SYNTAX;
		break;
	    }
            continue;
	}
        for (q = p; (*q >= ' ') && !isspace (*q); q++);
        for (*q++ = 0; (*q >= ' ') && isspace (*q); q++);

        if (! strcmp (p, "/cd"))
        {
            if (sstring (q, cdir, 1024) == 0) stat = ERR_PARAM;
        }	
        else if (! strcmp (p, "/convolver/new"))   stat = convnew (q, lnum);
        else if (! strcmp (p, "/impulse/read"))    stat = readfile (q, lnum, cdir);
        else if (! strcmp (p, "/impulse/dirac"))   stat = impdirac (q, lnum); 
        else if (! strcmp (p, "/impulse/hilbert")) stat = imphilbert (q, lnum); 
        else if (! strcmp (p, "/impulse/copy"))    stat = impcopy (q, lnum); 
        else if (! strcmp (p, "/input/name"))      stat = inpname (q); 
        else if (! strcmp (p, "/output/name"))     stat = outname (q); 
        else stat = ERR_COMMAND;
    }

    fclose (F);
    if (stat == ERR_OTHER) stat = 0;
    if (stat)
    {
	fprintf (stderr, "Line %d: ", lnum);
	switch (stat)
	{
	case ERR_SYNTAX:
	    fprintf (stderr, "Syntax error.\n");
	    break;
	case ERR_PARAM:
	    fprintf (stderr, "Bad or missing parameters.\n");
	    break;
	case ERR_ALLOC:
	    fprintf (stderr, "Out of memory.\n");
	    break;
	case ERR_CANTCD:
	    fprintf (stderr, "Can't change directory to '%s'.\n", cdir);
	    break;
	case ERR_COMMAND:
	    fprintf (stderr, "Unknown command.\n");
	    break;
	case ERR_NOCONV:
	    fprintf (stderr, "No convolver yet defined.\n");
	    break;
	case ERR_IONUM:
	    fprintf (stderr, "Bad input or output number.\n");
	    break;
	default:
	    fprintf (stderr, "Unknown error.\n");		     
	}
    }

    return stat;
}

