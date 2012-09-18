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

#ifndef FOLVE_STATUS_SERVER_H
#define FOLVE_STATUS_SERVER_H

#include <string>
#include <deque>

#include "file-handler-cache.h"
#include "file-handler.h"

#include <boost/thread/mutex.hpp>

class FolveFilesystem;
struct MHD_Daemon;
struct MHD_Connection;

class StatusServer : protected FileHandlerCache::Observer {
public:
  // Does not take over ownership of the filesystem.
  StatusServer(FolveFilesystem *fs);

  // Start server, listing on given port.
  bool Start(int port);

  // Shut down daemon.
  virtual ~StatusServer();

private:
  const std::string &CreatePage();

  static int HandleHttp(void* user_argument,
                        struct MHD_Connection *,
                        const char *, const char *, const char *,
                        const char *, size_t *, void **);

  void AppendFilterOptions(std::string *result);

  // Set filter or debug mode from http-request. Gracefully handles garbage.
  void SetFilter(const char *filter);
  void SetDebug(const char *filter);

  // -- interface FileHandlerCache::Observer
  virtual void InsertHandlerEvent(FileHandler *handler) {}
  virtual void RetireHandlerEvent(FileHandler *handler);
  
  typedef std::deque<HandlerStats> RetiredList;
  RetiredList retired_;
  int expunged_retired_;
  boost::mutex retired_mutex_;

  double total_seconds_filtered_;
  double total_seconds_music_seen_;
  FolveFilesystem *filesystem_;
  struct MHD_Daemon *daemon_;
  std::string content_;
  bool filter_switched_;
};

#endif  // FOLVE_STATUS_SERVER_H
