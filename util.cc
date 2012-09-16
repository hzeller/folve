//  Copyright (C) 2012 Henner Zeller <h.zeller@acm.org>
//    
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

double folve::CurrentTime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

void folve::Appendf(std::string *str, const char *format, ...) {
  va_list ap;
  const size_t orig_len = str->length();
  const size_t space = 1024;
  str->resize(orig_len + space);
  va_start(ap, format);
  int written = vsnprintf((char*)str->data() + orig_len, space, format, ap);
  va_end(ap);
  str->resize(orig_len + written);
}
