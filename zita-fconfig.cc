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


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "zita-config.h"


int convnew (const char *line, int lnum)
{
    unsigned int part;
    float        dens;
    int          r;

    r = sscanf (line, "%u %u %u %u %f", &ninp, &nout, &part, &size, &dens);
    if (r < 4) return ERR_PARAM;
    if (r < 5) dens = 0;

    if ((ninp == 0) || (ninp > Convproc::MAXINP))
    {
        fprintf (stderr, "Line %d: Number of inputs (%d) is out of range.\n", lnum, ninp);
        return ERR_OTHER;
    }
    if ((nout == 0) || (nout > Convproc::MAXOUT))
    {
        fprintf (stderr, "Line %d: Number of outputs (%d) is out of range.\n", lnum, nout);
        return ERR_OTHER;
    }
    if (size > MAXSIZE)
    {
        fprintf (stderr, "Line %d: Convolver size (%d) is out of range.\n", lnum, size);
        return ERR_OTHER;
    }
    if ((dens < 0.0f) || (dens > 1.0f))
    {
        fprintf (stderr, "Line %d: Density parameter is out of range.\n", lnum);
        return ERR_OTHER;
    }

    fragm = Convproc::MAXQUANT;
    while ((fragm > Convproc::MINPART) && (fragm >= 2 * size)) fragm /= 2;

    convproc->set_options (options);
    convproc->set_density (dens);
    if (convproc->configure (ninp, nout, size, fragm, fragm, fragm))
    {   
        fprintf (stderr, "Can't initialise convolution engine\n");
        return ERR_OTHER;
    }

    return 0;
}


int inpname (const char *)
{
    return 0;
}


int outname (const char *)
{
    return 0;
}

