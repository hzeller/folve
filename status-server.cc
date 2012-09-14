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

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <microhttpd.h>

#include "status-server.h"
#include "convolver-filesystem.h"

int StatusServer::HandleHttp(void* user_argument,
                             struct MHD_Connection *connection,
                             const char *url, const char *method,
                             const char *version,
                             const char *upload_data, size_t *upload_size,
                             void**) {
  StatusServer* server = (StatusServer*) user_argument;
  struct MHD_Response *response;
  int ret;
  const char *buffer;
  size_t size;
  server->CreatePage(&buffer, &size);
  response = MHD_create_response_from_data(size, (void*) buffer,
                                           MHD_NO, MHD_NO);
  MHD_add_response_header(response, "Content-Type", "text/html");
  ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  MHD_destroy_response(response);
  return ret;
}

StatusServer::StatusServer(ConvolverFilesystem *fs)
  : filesystem_(fs), daemon_(NULL) {
}

bool StatusServer::Start(int port) {
  daemon_ = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, port, NULL, NULL, 
                             &HandleHttp, this,
                             MHD_OPTION_END);
  return daemon_ != NULL;
}

StatusServer::~StatusServer() {
  if (daemon_)
    MHD_stop_daemon(daemon_);
}

static const char sMessageRowHtml[] = "<td>%s</td><td>%s</td>"
  "<td colspan='3'>-</td>";

static const char sProgressRowHtml[] =
  "<td>%s</td><td>"
  "<div style='width:%dpx; border:1px solid black;'>\n"
  "  <div style='width:%d%%;background:#7070ff;'>&nbsp;</div>\n</div></td>"
  "<td align='right'>%2d:%02d</td><td>/</td><td align='right'>%2d:%02d</td>";

static void AppendFileInfo(std::string *result, const std::string &filename,
                           const FileHandler *handler, int refs) {
  result->append("<tr>");
  char row[1024];
  const int kMaxWidth = 400;
  const float progress = handler->Progress();
  const char *access_level = (refs == 1) ? "idle" : "open";
  if (progress <= 0) {
    snprintf(row, sizeof(row), sMessageRowHtml, access_level,
             (progress < 0) ? "(not a sound file)" : "Only Header accessed.");
  } else {
    const int secs = handler->Duration();
    const int fract_sec = progress * secs;
    snprintf(row, sizeof(row), sProgressRowHtml, access_level,
             kMaxWidth, (int) (100 * progress),
             fract_sec / 60, fract_sec % 60, secs / 60, secs % 60);
  }
  result->append(row);
  result->append("<td bgcolor='#c0c0c0'>&nbsp;")
    .append(handler->FileInfo()).append("&nbsp;</td>")
    .append("<td>").append(filename).append("</td>");
  result->append("</tr>\n");
}


void StatusServer::CreatePage(const char **buffer, size_t *size) {
  current_page_.clear();
  current_page_.append("<body style='font-family:Helvetica;'>");
  current_page_.append("<center>Welcome to fuse convolve ")
    .append(filesystem_->version()).append("</center>");
  current_page_.append("<h3>Recent Files</h3>\n");
  FileHandlerCache::EntryList entries;
  filesystem_->handler_cache()->GetStats(&entries);
  current_page_.append("<table>\n");
  for (size_t i = 0; i < entries.size(); ++i) {
    const FileHandlerCache::Entry *entry = entries[i];
    AppendFileInfo(&current_page_, entry->key, entry->handler,
                   entry->references);
    filesystem_->handler_cache()->Unpin(entry->key);
  }
  current_page_.append("</table><hr/>\n");
  char file_openings[128];
  snprintf(file_openings, sizeof(file_openings),
           "Total inactive open %d | total re-open while active %d",
           filesystem_->total_file_openings(),
           filesystem_->total_file_reopen());
  current_page_.append(file_openings);
  current_page_.append("<div align='right'>HZ</div></body>");
  *buffer = current_page_.data();
  *size = current_page_.size();
}
