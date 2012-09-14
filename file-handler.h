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

#include "filter-interface.h"

class FileHandler : public filter_object_t {
public:
  // Returns bytes read or a negative value indicating a negative errno.
  virtual int Read(char *buf, size_t size, off_t offset) = 0;
  virtual int Stat(struct stat *st) = 0;
  virtual int Close() = 0;
  virtual ~FileHandler() {}
};
