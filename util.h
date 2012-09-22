// -*- c++ -*-
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

#ifndef FOLVE_UTIL_H
#define FOLVE_UTIL_H

#include <string>

  // Define this with empty, if you're not using gcc.
#define PRINTF_FMT_CHECK(fmt_pos, args_pos) \
    __attribute__ ((format (printf, fmt_pos, args_pos)))

// Some utility functions needed everywhere.
namespace folve {
  // Returns the current time as seconds since the start of the unix epoch,
  // but in microsecond resolution.
  double CurrentTime();

  // Like snprintf, but print to a std::string instead.
  void Appendf(std::string *str, const char *format, ...) PRINTF_FMT_CHECK(2,3);

  // Convenience, that returns a string directly. A bit less efficient than
  // Appendf().
  std::string StringPrintf(const char *format, ...) PRINTF_FMT_CHECK(1, 2);

  // Return if "str" has suffix "suffix".
  bool HasSuffix(const std::string &str, const std::string &suffix);

  // Log formatted string if debugging enabled.
  void DLogf(const char *format, ...) PRINTF_FMT_CHECK(1, 2);
  void EnableDebugLog(bool b);
  bool IsDebugLogEnabled();
}  // namespece folve

#undef PRINTF_FMT_CHECK

#endif  // FOLVE_UTIL_H
