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

#ifndef FUSE_CONVOLVER_STATUS_SERVER_H
#define FUSE_CONVOLVER_STATUS_SERVER_H

#include <string>
#include <deque>

#include "file-handler-cache.h"
#include "file-handler.h"

class ConvolverFilesystem;
struct MHD_Daemon;
struct MHD_Connection;

class StatusServer : protected FileHandlerCache::Observer {
public:
  // Does not take over ownership of the filesystem.
  StatusServer(ConvolverFilesystem *fs);
  bool Start(int port);

  virtual ~StatusServer();

private:
  static int HandleHttp(void* user_argument,
                        struct MHD_Connection *,
                        const char *, const char *, const char *,
                        const char *, size_t *, void **);

  void CreatePage(const char **buffer, size_t *size);

  // -- interface FileHandlerCache::Observer
  virtual void InsertHandlerEvent(FileHandler *handler) {}
  virtual void RetireHandlerEvent(FileHandler *handler);
  
  typedef std::deque<HandlerStats> RetiredList;
  RetiredList retired_;
  double total_seconds_filtered_;
  double total_seconds_music_seen_;
  ConvolverFilesystem *filesystem_;
  struct MHD_Daemon *daemon_;
  std::string current_page_;
};

#endif  // FUSE_CONVOLVER_STATUS_SERVER_H
