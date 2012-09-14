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

#include <string>

class ConvolverFilesystem;
struct MHD_Daemon;
struct MHD_Connection;

class StatusServer {
public:
  // Does not take over ownership of the filesystem.
  StatusServer(ConvolverFilesystem *fs);
  bool Start(int port);

  ~StatusServer();

private:
  static int HandleHttp(void* user_argument,
                        struct MHD_Connection *,
                        const char *, const char *, const char *,
                        const char *, size_t *, void **);

  void CreatePage(const char **buffer, size_t *size);

  ConvolverFilesystem *filesystem_;
  struct MHD_Daemon *daemon_;
  std::string current_page_;
};
