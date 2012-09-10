// -------------------------------------------------------------------------
//
//    Copyright (C) 2009-2010 Fons Adriaensen <fons@linuxaudio.org>
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


#ifndef __SSTRING_H
#define __SSTRING_H


// Scan 'srce' for a possibly quoted string, returning the
// result in 'dest'. At most size-1 characters will be put
// into 'dest'; in all cases a terminating zero is added.
// Leading spaces and tabs are skipped. The string can be
// surrounded by either single or double quotes which will
// not be copied to 'dest'.
// Control characters terminate the input unconditionally.
// Spaces and tabs (which will be converted to a space) are
// accepted if the input is quoted or when escaped, and 
// terminate the input otherwise.
// A '\' escapes the following character which means the '\'
// itself will not be inserted into 'dest', but the following
// char will be inserted even if it is a quote or a space, and
// will not terminate the input.
// Escapes are not accepted within a single-quoted string.
//
// Return value: the number of characters from 'srce' that
// were used, or 0 in case of any error. 

extern int sstring (const char *srce, char *dest, int size);


#endif
