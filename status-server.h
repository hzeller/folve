// -*- c++ -*-
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

#ifndef FOLVE_STATUS_SERVER_H
#define FOLVE_STATUS_SERVER_H

#include <microhttpd.h>

#include <string>
#include <deque>

#include "file-handler-cache.h"
#include "file-handler.h"
#include "util.h"

class FolveFilesystem;
struct MHD_Daemon;
struct MHD_Connection;

class StatusServer : protected FileHandlerCache::Observer {
public:
  // Does not take over ownership of the filesystem.
  StatusServer(FolveFilesystem *fs);
  virtual ~StatusServer();    // Shut down daemon.

  // Start server, listing on given port.
  bool Start(int port);

  // A file handler, that provides the current status as HTML-file. This
  // allows to acces the current status even if there is no status port.
  FileHandler *CreateStatusFileHandler();

  // Set browser meta-refresh time. < 0 to disable.
  void set_meta_refresh(int seconds) { meta_refresh_time_ = seconds; }

private:
  class HtmlFileHandler;
  friend class HtmlFileHandler;

#if MHD_VERSION >= 0x00097002
  typedef MHD_Result HandleHttpResult;
#else
  typedef int HandleHttpResult;
#endif

  // micro-httpd callback
  static HandleHttpResult HandleHttp(void* user_argument,
                                     struct MHD_Connection *,
                                     const char *, const char *, const char *,
                                     const char *, size_t *, void **);

  void CreatePage(bool for_http, std::string *content);

  const std::string &CreateHttpPage();

  // Some helper functions to create the page:
  void AppendSettingsForm(bool for_http, std::string *out);
  void AppendFileInfo(const char *progress_access_color,
                      const char *progress_buffer_color,
                      const HandlerStats &stats,
                      std::string *out);

  // Set filter from http-request. Gracefully handles garbage.
  void SetFilter(const char *value);

  // Show details that might only be interesting while setting up things.
  bool show_details();

  // -- interface FileHandlerCache::Observer
  virtual void InsertHandlerEvent(FileHandler *handler) {}
  virtual void RetireHandlerEvent(FileHandler *handler);

  typedef std::deque<HandlerStats> RetiredList;
  RetiredList retired_;
  int expunged_retired_;
  folve::Mutex retired_mutex_;

  int meta_refresh_time_;
  FolveFilesystem *filesystem_;
  struct MHD_Daemon *daemon_;
  std::string http_content_;
  bool filter_switched_;
};

#endif  // FOLVE_STATUS_SERVER_H
