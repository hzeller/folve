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

#include "convolver-filesystem.h"
#include "status-server.h"
#include "util.h"

static const size_t kMaxRetired = 200;
static const int kProgressWidth = 400;
static const char kActiveProgress[]  = "#7070ff";
static const char kRetiredProgress[] = "#d0d0d0";

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
  fs->handler_cache()->SetObserver(this);
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

// FileHandlerCache::Observer interface.
void StatusServer::RetireHandlerEvent(FileHandler *handler) {
  HandlerStats stats;
  handler->GetHandlerStatus(&stats);  // Get last available stats.
  if (stats.progress >= 0) {
    total_seconds_music_seen_ += stats.total_duration_seconds;
    total_seconds_filtered_ += stats.total_duration_seconds * stats.progress;
  }
  stats.last_access = fuse_convolve::CurrentTime();
  stats.status = HandlerStats::RETIRED;
  retired_.push_front(stats);
  while (retired_.size() > kMaxRetired)
    retired_.pop_back();
}

static const char sMessageRowHtml[] = "<td>%s</td><td>%s</td>"
  "<td colspan='3'>-</td>";

static const char sProgressRowHtml[] =
  "<td>%s</td><td>"
  "<div style='width:%dpx; border:1px solid black;'>\n"
  "  <div style='width:%d%%;background:%s;'>&nbsp;</div>\n</div></td>"
  "<td align='right'>%2d:%02d</td><td>/</td><td align='right'>%2d:%02d</td>";

static void AppendFileInfo(std::string *result, const char *progress_style,
                           const HandlerStats &stats) {
  result->append("<tr>");
  char row[1024];
  const char *status = "";
  switch (stats.status) {
  case HandlerStats::OPEN:    status = "open"; break;
  case HandlerStats::IDLE:    status = "idle"; break;
  case HandlerStats::RETIRED: status = "----"; break;
    // no default to let the compiler detect new values.
  }
  if (stats.progress <= 0) {
    snprintf(row, sizeof(row), sMessageRowHtml, status,
             (stats.progress < 0)
             ? "Not a sound file or no filter found. Pass through."
             : "Only Header accessed.");
  } else {
    const int secs = stats.total_duration_seconds;
    const int fract_sec = stats.progress * secs;
    snprintf(row, sizeof(row), sProgressRowHtml, status,
             kProgressWidth, (int) (100 * stats.progress), progress_style,
             fract_sec / 60, fract_sec % 60, secs / 60, secs % 60);
  }
  result->append(row);
  result->append("<td bgcolor='#c0c0c0'>&nbsp;")
    .append(stats.format).append("&nbsp;</td>")
    .append("<td style='font-size:small;'>").append(stats.filename)
    .append("</td>");
  result->append("</tr>\n");
}

struct CompareStats {
  bool operator() (const HandlerStats &a, const HandlerStats &b) {
    if (a.status < b.status) return true;   // open before idle.
    else if (a.status > b.status) return false;
    return b.last_access < a.last_access;   // reverse time.
  }
};

void StatusServer::CreatePage(const char **buffer, size_t *size) {
  const double start = fuse_convolve::CurrentTime();
  current_page_.clear();
  current_page_.append("<body style='font-family:Helvetica;'>");
  current_page_.append("<center>Welcome to fuse convolve ")
    .append(filesystem_->version()).append("</center>");

  std::vector<HandlerStats> stat_list;
  filesystem_->handler_cache()->GetStats(&stat_list);

  // Get statistics of active files.
  double active_music_seen = 0.0;
  double active_filtered = 0.0;
  for (size_t i = 0; i < stat_list.size(); ++i) {
    const HandlerStats &stats = stat_list[i];
    if (stats.progress >= 0) {
      active_music_seen += stats.total_duration_seconds;
      active_filtered += stats.total_duration_seconds * stats.progress;
    }
  }
  const int t_seen = total_seconds_music_seen_ + active_music_seen;
  const int t_filtered = total_seconds_filtered_ + active_filtered;
  char total_stats[1024];
  snprintf(total_stats, sizeof(total_stats),
           "Total opening files <b>%d</b> | "
           ".. and re-opened from recency cache <b>%d</b><br/>"
           "Total music seen <b>%d:%02d:%02d</b> | "
           ".. and convolved <b>%d:%02d:%02d</b><br/>",
           filesystem_->total_file_openings(),
           filesystem_->total_file_reopen(),
           t_seen / 3600, (t_seen % 3600) / 60, t_seen % 60,
           t_filtered / 3600, (t_filtered % 3600) / 60, t_filtered % 60);
  current_page_.append(total_stats);

  current_page_.append("<h3>Recent Files</h3>\n");
  char current_open[128];
  snprintf(current_open, sizeof(current_open), "%ld in recency cache.\n",
           stat_list.size());

  current_page_.append(current_open);
  current_page_.append("<table>\n");
  current_page_.append("<tr><th>Stat</th>"
                       "<th width='400px'>Progress</th>"
                       "<th>Pos</th><td></td><th>Tot</th><th>Format</th>"
                       "<th>File</th></tr>\n");
  CompareStats comparator;
  std::sort(stat_list.begin(), stat_list.end(), comparator);
  for (size_t i = 0; i < stat_list.size(); ++i) {
    AppendFileInfo(&current_page_, kActiveProgress, stat_list[i]);
  }
  current_page_.append("</table><hr/>\n");

  if (retired_.size() > 0) {
    current_page_.append("<table>\n");
    current_page_.append("<h3>Retired</h3>\n");
    for (RetiredList::const_iterator it = retired_.begin();
         it != retired_.end(); ++it) {
      AppendFileInfo(&current_page_, kRetiredProgress, *it);
    }
    current_page_.append("</table><hr/>\n");
  }

  const double duration = fuse_convolve::CurrentTime() - start;
  char time_buffer[128];
  snprintf(time_buffer, sizeof(time_buffer), "page-gen %.2fms",
           duration * 1000.0);
  current_page_.append(time_buffer).append("<div align='right'>HZ</div></body>");
  *buffer = current_page_.data();
  *size = current_page_.size();
}
