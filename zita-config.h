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


#ifndef __CONFIG_H
#define __CONFIG_H


#include <zita-convolver.h>
#include "zita-sstring.h"

struct ZitaConfig {
  Convproc *convproc;
  unsigned int  latency;
  unsigned int  options;
  unsigned int  fsamp;
  unsigned int  fragm;
  unsigned int  ninp;
  unsigned int  nout;
  unsigned int  size;
  const char *config_file;
};

enum { NOERR, ERR_OTHER, ERR_SYNTAX, ERR_PARAM, ERR_ALLOC, ERR_CANTCD, ERR_COMMAND, ERR_NOCONV, ERR_IONUM };


extern int  config (ZitaConfig *cfg, const char *config_file);
extern int  convnew (ZitaConfig *cfg, const char *line, int lnum);
extern int  inpname (ZitaConfig *cfg, const char *line);
extern int  outname (ZitaConfig *cfg, const char *line);
extern void makeports (void);


#define MAXSIZE 0x00100000


#endif


