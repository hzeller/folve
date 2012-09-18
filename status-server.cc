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
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <microhttpd.h>

#include <boost/thread/locks.hpp>

#include "folve-filesystem.h"
#include "status-server.h"
#include "util.h"

using folve::Appendf;

// TODO: someone with a bit more stylesheet-fu can attempt to make this
// more pretty and the HTML more compact.

static const size_t kMaxRetired = 200;
static const int kProgressWidth = 300;
static const char kActiveProgress[]  = "#7070ff";
static const char kRetiredProgress[] = "#d0d0d0";
static const char kSettingsUrl[] = "/settings";

// Aaah, I need to find the right Browser-Tab :)
// Sneak in a favicon without another resource access.
// TODO: make a nice icon, recognizable as something that has to do with "
// files and music ...
static const char kHtmlHeader[] = "<header>"
  "<title>Folve</title>\n"
  "<link rel='icon' type='image/png'"
  "href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2"
  "AAAAAXNSR0IArs4c6QAAAAlwSFlzAAALEwAACxMBAJqcGAAAAAd0SU1FB9wJDwUlEA/UBrsA"
  "AABSSURBVCjPrZIxDgAgDAKh8f9froOTirU1ssKFYqS7Q4mktAxFRQDJcsPORMDYsDCXhn331"
  "9GPwHJVuaFl3l4D1+h0UjIdbTh9SpP2KQ2AgSfVAdEQGx23tOopAAAAAElFTkSuQmCC'/>\n"
  "</header>";

// Callback function called by micro http daemon. Gets the StatusServer pointer
// in the user_argument.
int StatusServer::HandleHttp(void* user_argument,
                             struct MHD_Connection *connection,
                             const char *url, const char *method,
                             const char *version,
                             const char *upload_data, size_t *upload_size,
                             void**) {
  StatusServer* server = (StatusServer*) user_argument;
  
  if (strcmp(url, kSettingsUrl) == 0) {
    server->SetFilter(MHD_lookup_connection_value(connection,
                                                  MHD_GET_ARGUMENT_KIND, "f"));
    server->SetDebug(MHD_lookup_connection_value(connection,
                                                 MHD_GET_ARGUMENT_KIND, "d"));
  }

  struct MHD_Response *response;
  int ret;
  const std::string &page = server->CreatePage();
  response = MHD_create_response_from_data(page.length(), (void*) page.data(),
                                           MHD_NO, MHD_NO);
  MHD_add_response_header(response, "Content-Type", "text/html; charset=utf-8");
  ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  MHD_destroy_response(response);
  return ret;
}

StatusServer::StatusServer(FolveFilesystem *fs)
  : expunged_retired_(0), total_seconds_filtered_(0),
    total_seconds_music_seen_(0),
    filesystem_(fs), daemon_(NULL), filter_switched_(false) {
  fs->handler_cache()->SetObserver(this);
}

void StatusServer::SetFilter(const char *filter) {
  if (filter == NULL || *filter == '\0') return;
  char *end;
  int index = strtol(filter, &end, 10);
  if (end == NULL || *end != '\0') return;
  filter_switched_ = (index != filesystem_->current_cfg_index());
  filesystem_->SwitchCurrentConfigIndex(index);
}

void StatusServer::SetDebug(const char *dbg) {
  if (!filesystem_->is_debug_ui_enabled()) return;
  filesystem_->SetDebugMode(dbg != NULL && *dbg == '1');
}

bool StatusServer::Start(int port) {
  daemon_ = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, port, NULL, NULL, 
                             &HandleHttp, this,
                             MHD_OPTION_END);
  return daemon_ != NULL;
}

StatusServer::~StatusServer() {
  if (daemon_) MHD_stop_daemon(daemon_);
}

// FileHandlerCache::Observer interface.
void StatusServer::RetireHandlerEvent(FileHandler *handler) {
  HandlerStats stats;
  handler->GetHandlerStatus(&stats);  // Get last available stats.
  if (stats.progress >= 0) {
    total_seconds_music_seen_ += stats.duration_seconds;
    total_seconds_filtered_ += stats.duration_seconds * stats.progress;
  }
  stats.last_access = folve::CurrentTime();
  stats.status = HandlerStats::RETIRED;
  boost::lock_guard<boost::mutex> l(retired_mutex_);
  retired_.push_front(stats);
  while (retired_.size() > kMaxRetired) {
    ++expunged_retired_;
    retired_.pop_back();
  }
}
               
static const char sMessageRowHtml[] =
  "<td>%s</td><td style='font-size:small;'>%s</td>"
  "<td colspan='3' align='center'>-</td>";

static const char sProgressRowHtml[] =
  "<td>%s</td><td>"
  "<div style='width:%dpx; border:1px solid black;'>\n"
  "  <div style='width:%d%%;background:%s;'>&nbsp;</div>\n</div></td>"
  "<td align='right'>%2d:%02d</td><td>/</td><td align='right'>%2d:%02d</td>";

static void AppendFileInfo(std::string *result, const char *progress_style,
                           const HandlerStats &stats) {
  result->append("<tr style='text-wrap:none;white-space:nowrap;'>");
  const char *status = "";
  switch (stats.status) {
  case HandlerStats::OPEN:    status = "open"; break;
  case HandlerStats::IDLE:    status = "idle"; break;
  case HandlerStats::RETIRED: status = "&nbsp;----&nbsp;"; break;
    // no default to let the compiler detect new values.
  }
  if (!stats.message.empty()) {
    Appendf(result, sMessageRowHtml, status, stats.message.c_str());
  } else if (stats.progress == 0) {
    Appendf(result, sMessageRowHtml, status, "Only header accessed");
  } else {
    const int secs = stats.duration_seconds;
    const int fract_sec = stats.progress * secs;
    Appendf(result, sProgressRowHtml, status,
            kProgressWidth, (int) (100 * stats.progress), progress_style,
            fract_sec / 60, fract_sec % 60, secs / 60, secs % 60);
  }
  Appendf(result, "<td bgcolor='#c0c0c0'>&nbsp;%s&nbsp;</td>",
          stats.format.c_str());
  Appendf(result,"<td "
          "style='font-size:small;text-wrap:none;white-space:nowrap'>%s</td>",
          stats.filename.c_str());
  result->append("</tr>\n");
}

void StatusServer::AppendFilterOptions(std::string *result) {
  Appendf(result, "<form action='%s'>"
          "<label for='cfg_sel'>Config directory</label> ", kSettingsUrl);
  result->append("<select id='cfg_sel' name='f' "
                 "onchange='this.form.submit();'>\n");
  for (size_t i = 0; i < filesystem_->config_dirs().size(); ++i) {
    const std::string &c = filesystem_->config_dirs()[i];
    Appendf(result, "<option value='%zd'%s>%s</option>\n",
            i, ((int) i == filesystem_->current_cfg_index()) ? " selected" : "",
            (i == 0) ? "[No convolver - just pass through]" : c.c_str());
  }
  result->append("</select>");
  if (filesystem_->config_dirs().size() == 1) {
    result->append(" (This is a boring configuration, add filter directories "
                   "with -c &lt;dir&gt; [-c &lt;another-dir&gt; ...] :-) )");
  } else if (filter_switched_) {
    result->append(" <span style='font-size:small;'>Affects re- or newly opened "
                   "files.</span>");
  }
  if (filesystem_->is_debug_ui_enabled()) {
    Appendf(result, "<span style='float:right;font-size:small;'>"
            "<label for='dbg_sel'>Folve debug to syslog</label>"
            "<input id='dbg_sel' onchange='this.form.submit();' "
            "type='checkbox' name='d' value='1'%s/></span>",
            filesystem_->IsDebugMode() ? " checked" : "");
  }
  result->append("</form>");
}

struct CompareStats {
  bool operator() (const HandlerStats &a, const HandlerStats &b) {
    if (a.status < b.status) return true;   // open before idle.
    else if (a.status > b.status) return false;
    return b.last_access < a.last_access;   // reverse time.
  }
};

const std::string &StatusServer::CreatePage() {
  const double start = folve::CurrentTime();
  // We re-use a string to avoid re-allocing memory every time we generate
  // a page. Since we run with MHD_USE_SELECT_INTERNALLY, this is only accessed
  // by one thread.
  content_.clear();
  content_.append(kHtmlHeader);
  content_.append("<body style='font-family:Sans-Serif;'>\n");
  Appendf(&content_, "<center style='background-color:#A0FFA0;'>"
          "Welcome to "
          "<a href='https://github.com/hzeller/folve#readme'>Folve</a> "
          FOLVE_VERSION "</center>\n"
          "Convolving audio files from <code>%s</code>\n",
          filesystem_->underlying_dir().c_str());

  AppendFilterOptions(&content_);

  std::vector<HandlerStats> stat_list;
  filesystem_->handler_cache()->GetStats(&stat_list);

  // Get statistics of active files to add to the existing ones.
  double active_music_seen = 0.0;
  double active_filtered = 0.0;
  for (size_t i = 0; i < stat_list.size(); ++i) {
    const HandlerStats &stats = stat_list[i];
    if (stats.progress >= 0) {
      active_music_seen += stats.duration_seconds;
      active_filtered += stats.duration_seconds * stats.progress;
    }
  }
  const int t_seen = total_seconds_music_seen_ + active_music_seen;
  const int t_filtered = total_seconds_filtered_ + active_filtered;
  Appendf(&content_, "Total opening files <b>%d</b> "
          ".. and re-opened from recency cache <b>%d</b><br/>",
          filesystem_->total_file_openings(),
          filesystem_->total_file_reopen());
  Appendf(&content_, "Total music seen <b>%dd %d:%02d:%02d</b> ",
          t_seen / 86400, (t_seen % 86400) / 3600,
          (t_seen % 3600) / 60, t_seen % 60);
  Appendf(&content_, ".. and convolved <b>%dd %d:%02d:%02d</b> ",
          t_filtered / 86400, (t_filtered % 86400) / 3600,
          (t_filtered % 3600) / 60, t_filtered % 60);
  Appendf(&content_, "(%.1f%%)<br/>", 
          (t_seen == 0) ? 0.0 : (100.0 * t_filtered / t_seen));

  Appendf(&content_, "<h3>Accessed Recently</h3>\n%zd in recency cache\n",
          stat_list.size());

  content_.append("<table>\n");
  Appendf(&content_, "<tr><th>Stat</th><th width='%dpx'>Progress</th>"
          "<th>Pos</th><td></td><th>Len</th><th>Format</th>"
          "<th align='left'>File</th></tr>\n", kProgressWidth);
  CompareStats comparator;
  std::sort(stat_list.begin(), stat_list.end(), comparator);
  for (size_t i = 0; i < stat_list.size(); ++i) {
    AppendFileInfo(&content_, kActiveProgress, stat_list[i]);
  }
  content_.append("</table><hr/>\n");

  if (retired_.size() > 0) {
    content_.append("<h3>Retired</h3>\n");
    content_.append("<table>\n");
    boost::lock_guard<boost::mutex> l(retired_mutex_);
    for (RetiredList::const_iterator it = retired_.begin(); 
         it != retired_.end(); ++it) {
      AppendFileInfo(&content_, kRetiredProgress, *it);
    }
    content_.append("</table>\n");
    if (expunged_retired_ > 0) {
      Appendf(&content_, "... (%d more)<p></p>", expunged_retired_);
    }
    content_.append("<hr/>");
  }

  const double duration = folve::CurrentTime() - start;
  Appendf(&content_,
          "<span style='float:left;font-size:small;'>page-gen %.2fms</span>"
          "<span style='float:right;font-size:small;'>HZ</span>"
          "</body>", duration * 1000.0);
  return content_;
}
