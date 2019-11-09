//  -----------------------------------------------------------------------------
//
//  Copyright (C) 2006-2011 Fons Adriaensen <fons@linuxaudio.org>
//
//  [ This derivative is based on fconfig.cc in jconvolver 0.9.2.
//
//  Modifications to work with Folve, most notably allowing to have multiple
//  concurrent configurations loaded and convolvers active.
//  Logging of outputs with syslog() instead of fprintf():
//  Copyright (C) 2012 Henner Zeller <h.zeller@acm.org>
//  ]
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
#include <syslog.h>

#include "zita-config.h"


int convnew (ZitaConfig *cfg, const char *line, int lnum)
{
    unsigned int part;
    float        dens;
    int          r;

    r = sscanf (line, "%u %u %u %u %f", &cfg->ninp, &cfg->nout,
                &part, &cfg->size, &dens);
    if (r < 4) return ERR_PARAM;
    if (r < 5) dens = 0;

    if ((cfg->ninp == 0) || (cfg->ninp > Convproc::MAXINP))
    {
        syslog(LOG_ERR, "%s:%d: Number of inputs (%d) is out of range.\n",
                 cfg->config_file, lnum, cfg->ninp);
        return ERR_OTHER;
    }
    if ((cfg->nout == 0) || (cfg->nout > Convproc::MAXOUT))
    {
        syslog(LOG_ERR, "%s:%d: Number of outputs (%d) is out of range.\n",
                 cfg->config_file, lnum, cfg->nout);
        return ERR_OTHER;
    }
    if (cfg->size > MAXSIZE)
    {
        syslog(LOG_ERR, "%s:%d: Convolver size (%d) is out of range.\n",
                 cfg->config_file, lnum, cfg->size);
        return ERR_OTHER;
    }
    if ((dens < 0.0f) || (dens > 1.0f))
    {
        syslog(LOG_ERR, "%s:%d: Density parameter is out of range.\n",
                 cfg->config_file, lnum);
        return ERR_OTHER;
    }

    cfg->fragm = Convproc::MAXQUANT;
    while ((cfg->fragm > Convproc::MINPART) && (cfg->fragm >= 2 * cfg->size)) {
      cfg->fragm /= 2;
    }
    cfg->convproc->set_options (cfg->options);
#if ZITA_CONVOLVER_MAJOR_VERSION >= 4
    if (cfg->convproc->configure (cfg->ninp, cfg->nout, cfg->size,
                                  cfg->fragm, cfg->fragm, cfg->fragm, dens))
    {
        syslog(LOG_ERR, "Can't initialise convolution engine\n");
        return ERR_OTHER;
    }
#else
    cfg->convproc->set_density (dens);
    if (cfg->convproc->configure (cfg->ninp, cfg->nout, cfg->size,
                                  cfg->fragm, cfg->fragm, cfg->fragm))
    {
        syslog(LOG_ERR, "Can't initialise convolution engine\n");
        return ERR_OTHER;
    }
#endif

    return 0;
}


int inpname (ZitaConfig *, const char *)
{
    return 0;
}


int outname (ZitaConfig *, const char *)
{
    return 0;
}
