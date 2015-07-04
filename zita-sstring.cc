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


#include <ctype.h>


#define SQUOTE 39
#define DQUOTE 34
#define BSLASH 92


// See the header file for a description.

int sstring (const char *srce, char *dest, int size)
{
    int i, j;
    unsigned int c, ef, qf;

    if (size < 0) return 0;
    i = j = ef = qf = 0;
    while (true)
    {
	if (j == size)
	{
	    // Non-zero char in last position of dest, return error.
	    dest [0] = 0;
	    return 0;
	}
	c = srce [i++];
	// Tabs are converted to spaces.
	if (isblank (c)) c = ' ';
	if (iscntrl (c))
	{
	    // Control character. If within quotes or escaped return error,
	    // else terminate scanning.
	    if (qf | ef)
	    {
		dest [0] = 0;
                return 0;
	    }
            dest [j] = 0;
	    return --i;
	}
	if (ef)
	{
	    // Escaped character. If tab, insert space, else
            // insert the character as is. Reset escape flag.
	    dest [j++] = c;
	    ef = 0;
	    continue;
	}
	if (c == BSLASH)
	{
	    // An '\'. If in single quotes, treat as normal
            // character, else set the escape flag.
	    if (qf == SQUOTE) dest [j++] = c;
            else ef = c;
	    continue;
	}
        if ((c == SQUOTE) || (c == DQUOTE))
        {
	    // Single or double quote. If we have a leading quote,
            // terminate scanning if this one matches it, or return
            // error if not. Else set as leading quote.
    	    if (c == qf)
	    {
		dest [j] = 0;
		return i;
	    }
	    if (qf || j)
	    {
		dest [0] = 0;
		return 0;
	    }
            qf = c;
	    continue;
	}
	if (c == ' ')
	{
	    // Skip if leading space, insert if quoted, otherwise
            // terminate scanning.
	    if (qf)
	    {
		dest [j++] = ' ';
		continue;
	    }
	    if (j)
	    {
		dest [j++] = 0;
		return --i;
	    }
	    continue;
	}
	// Normal character.
	dest [j++] = c;
    }
    return 0;
}
