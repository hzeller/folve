//  -*- c++ -*-
//  Folve - A fuse filesystem that convolves audio files on-the-fly.
//
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
#ifndef FOLVE_PASS_THROUGH_HANDLER_H_
#define FOLVE_PASS_THROUGH_HANDLER_H_

#include "file-handler.h"

// Very simple file handler that just passes through the original file.
// Used for everything that is not a sound-file or for which no filter
// configuration could be found.
class PassThroughHandler : public FileHandler {
public:
  PassThroughHandler(int filedes, const std::string &filter_id,
                     const HandlerStats &known_stats);
  ~PassThroughHandler();

  virtual int Read(char *buf, size_t size, off_t offset);
  virtual int Stat(struct stat *st);
  virtual void GetHandlerStatus(HandlerStats *stats);
 
private:
  const int filedes_;
  size_t file_size_;
  long unsigned int max_accessed_;
  HandlerStats info_stats_;
};

#endif  // FOLVE_PASS_THROUGH_HANDLER_H_
