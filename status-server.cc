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

#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
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

static const size_t kMaxRetired = 20;
static const int kProgressWidth = 300;
static const char kActiveProgress[]  = "#7070ff";
static const char kRetiredProgress[] = "#d0d0d0";
static const char kSettingsUrl[] = "/settings";

// Aaah, I need to find the right Browser-Tab :)
// Sneak in a favicon without another resource access.
// TODO: make a nice icon, recognizable as something that has to do with "
// files and music ...
static const char kStartHtmlHeader[] = "<html><head>"
  "<title>Folve</title>\n"
  "<link rel='icon' type='image/png' "
  "href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2"
  "AAAAAXNSR0IArs4c6QAAAAlwSFlzAAALEwAACxMBAJqcGAAAAAd0SU1FB9wJDwUlEA/UBrsA"
  "AABSSURBVCjPrZIxDgAgDAKh8f9froOTirU1ssKFYqS7Q4mktAxFRQDJcsPORMDYsDCXhn331"
  "9GPwHJVuaFl3l4D1+h0UjIdbTh9SpP2KQ2AgSfVAdEQGx23tOopAAAAAElFTkSuQmCC'/>\n";

// TODO: someone with a bit more stylesheet-fu can attempt to make this
// more pretty and the HTML more compact.
// First step: extract all css used below in style='xx' here. here.
// Also, maybe move to external file, makes editing much faster, then compile
// that into a C-string that we can include it in the binary.
#define SELECT_COLOR "#a0a0ff"
#define PRE_SELECT_COLOR "#e0e0ff"
static const char kCSS[] =
  "<style type='text/css'>"
  " a:link { text-decoration:none; }\n"
  " a:visited { text-decoration:none; }\n"
  " a:hover { text-decoration:underline; }\n"
  " a:active { text-decoration:underline; }\n"
  " .filter_sel { font-weight:bold; \n"
  "                padding: 5px 15px;\n"
  "                border-radius: 5px;\n"
  "                -moz-border-radius: 5px; }\n"
  " .active { background-color:" SELECT_COLOR "; }\n"
  " .inactive { background-color:#e0e0e0; }\n"
  " .inactive:hover { background-color:" PRE_SELECT_COLOR ";\n"
  "                   color: #000000;\n"
  "                   text-decoration:none;}\n"
  " .inactive:link { color: #000000;text-decoration:none;}\n"
  " .inactive:visited { color: #000000;text-decoration:none;}\n"
  "</style>";

// Callback function called by micro http daemon. Gets the StatusServer pointer
// in the user_argument.
int StatusServer::HandleHttp(void* user_argument,
                             struct MHD_Connection *connection,
                             const char *url, const char *method,
                             const char *version,
                             const char *upload_data, size_t *upload_size,
                             void**) {
  StatusServer* server = (StatusServer*) user_argument;
  struct MHD_Response *response;
  int ret;

  if (strcmp(url, kSettingsUrl) == 0) {
    server->SetFilter(MHD_lookup_connection_value(connection,
                                                  MHD_GET_ARGUMENT_KIND, "f"));
    server->SetDebug(MHD_lookup_connection_value(connection,
                                                 MHD_GET_ARGUMENT_KIND, "d"));
    // We redirect to slash after this, to remove parameters from the GET-URL
    response = MHD_create_response_from_data(0, (void*)"", MHD_NO, MHD_NO);
    MHD_add_response_header(response, "Location", "/");
    ret = MHD_queue_response(connection, 302, response);    
  } else {
    const std::string &page = server->CreatePage();
    response = MHD_create_response_from_data(page.length(), (void*) page.data(),
                                             MHD_NO, MHD_NO);
    MHD_add_response_header(response, "Content-Type",
                            "text/html; charset=utf-8");
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  }
  // Tell aggressive cachers not to do so.
  MHD_add_response_header(response, "Cache-Control", "no-cache");
  MHD_add_response_header(response, "Expires", "24 Nov 1972 23:42:42 GMT");
  MHD_destroy_response(response);
  return ret;
}

StatusServer::StatusServer(FolveFilesystem *fs)
  : expunged_retired_(0), total_seconds_filtered_(0),
    total_seconds_music_seen_(0),
    meta_refresh_time_(-1),
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
  folve::EnableDebugLog(dbg != NULL && *dbg == '1');
}

bool StatusServer::Start(int port) {
  PrepareConfigDirectoriesForUI();
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
               
// As ugly #defines, so that gcc can warn about printf() format problems.
#define sMessageRowHtml \
  "<td>%s</td><td colspan='3' style='font-size:small;'>%s</td>"

#define sProgressRowHtml \
  "<td>%s</td>"  \
  "<td>%s</td>"  \
  "<td><div style='background:white;width:%dpx;border:1px solid black;'>\n" \
  "  <div style='width:%dpx;background:%s;'>&nbsp;</div>\n</div></td>" \
  "<td>%s</td>"

#define sTimeColumns \
  "<td align='right'>%2d:%02d</td><td>/</td><td align='right'>%2d:%02d</td>"
#define sDecibelColumn \
  "<td align='right' style='background:%s;'>%.1f dB</td>"

void StatusServer::AppendFileInfo(const char *progress_style,
                                  const HandlerStats &stats) {
  content_.append("<tr style='text-wrap:none;white-space:nowrap;'>");
  const char *status = "";
  switch (stats.status) {
  case HandlerStats::OPEN:    status = "open"; break;
  case HandlerStats::IDLE:    status = "idle"; break;
  case HandlerStats::RETIRED: status = "&nbsp;----&nbsp;"; break;
    // no default to let the compiler detect new values.
  }

  if (!stats.message.empty()) {
    Appendf(&content_, sMessageRowHtml, status, stats.message.c_str());
  } else if (stats.progress == 0) {
    Appendf(&content_, sMessageRowHtml, status, "Only header accessed");
  } else {
    Appendf(&content_, sProgressRowHtml, status,
            stats.in_gapless ? "&rarr;" : "",
            kProgressWidth, (int) (kProgressWidth * stats.progress),
            progress_style,
            stats.out_gapless ? "&rarr;" : "");
  }
  const int secs = stats.duration_seconds;
  const int fract_sec = stats.progress * secs;
  if (secs >= 0 && fract_sec >= 0) {
    Appendf(&content_, sTimeColumns, 
            fract_sec / 60, fract_sec % 60,
            secs / 60, secs % 60);
  } else {
    content_.append("<td colspan='3'>-</td>");
  }

  if (stats.max_output_value > 1e-6) {
    Appendf(&content_, sDecibelColumn,
            stats.max_output_value > 1.0 ? "#FF0505" : "white",
            20 * log10f(stats.max_output_value));
  } else {
    content_.append("<td>-</td>");
  }

  Appendf(&content_, "<td bgcolor='#c0c0c0'>&nbsp;%s (%s)&nbsp;</td>",
          stats.format.c_str(), ui_config_directories_[stats.filter_id].c_str());
  Appendf(&content_,"<td "
          "style='font-size:small;text-wrap:none;white-space:nowrap'>%s</td>",
          stats.filename.c_str());
  content_.append("</tr>\n");
}

void StatusServer::PrepareConfigDirectoriesForUI() {
  // Essentially only keep the directory name.
  if (!ui_config_directories_.empty()) return;
  ui_config_directories_.push_back("None : Pass Through");
  for (size_t i = 1; i < filesystem_->config_dirs().size(); ++i) {
    std::string d = filesystem_->config_dirs()[i];
    while (d.length() > 0 && d[d.length() - 1] == '/') {
      d.resize(d.length() - 1);   // trim trailing slashes.
    }
    const std::string::size_type slash_pos = d.find_last_of('/');
    if (slash_pos != std::string::npos) {
      d = d.substr(slash_pos + 1);
    }
    ui_config_directories_.push_back(d);
  }
}

static void CreateSelection(std::string *result,
                            const std::vector<std::string> &options,
                            int selected) {
  if (options.size() == 1) {
    result->append(options[0]);   // no reason to make this a form :)
    return;
  }
  for (size_t i = 0; i < options.size(); ++i) {
    const std::string &c = options[i];
    result->append("&nbsp;");
    const bool active = (int) i == selected;
    if (active) {
      Appendf(result, "<span class='filter_sel active'>%s</span>\n", c.c_str());
    } else {
      Appendf(result, "<a class='filter_sel inactive' href='%s?f=%zd'>%s</a>\n",
              kSettingsUrl, i, c.c_str());
    }
  }
}

void StatusServer::AppendSettingsForm() {
  content_.append("<p>Active filter: ");
  CreateSelection(&content_, ui_config_directories_,
                  filesystem_->current_cfg_index());
  if (filesystem_->config_dirs().size() == 1) {
    content_.append(" (This is a boring configuration, add filter directories "
                    "with -c &lt;dir&gt; [-c &lt;another-dir&gt; ...] :-) )");
  } else if (filter_switched_) {
    content_.append("&nbsp;<span style='font-size:small;background:#FFFFa0;'>"
                    " (Affects re- or newly opened files.) </span>");
    filter_switched_ = false;  // only show once.
  }
  // TODO: re-add something for filesystem_->is_debug_ui_enabled()
  content_.append("</p><hr/>");
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
  content_.append(kStartHtmlHeader);
  if (meta_refresh_time_ > 0) {
    Appendf(&content_, "<meta http-equiv='refresh' content='%d'>\n",
            meta_refresh_time_);
  }
  content_.append(kCSS);
  content_.append("</head>\n");

  content_.append("<body style='font-family:Sans-Serif;'>\n");
  Appendf(&content_, "<center style='background-color:#A0FFA0;'>"
          "Welcome to "
          "<a href='https://github.com/hzeller/folve#readme'>Folve</a> "
          FOLVE_VERSION "</center>\n"
          "Convolving audio files from <code>%s</code>\n",
          filesystem_->underlying_dir().c_str());

  AppendSettingsForm();

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

  if (filesystem_->gapless_processing()) {
    content_.append("<br/>&rarr; : denotes gapless transfers\n");
  }
  content_.append("<table>\n");
  Appendf(&content_, "<tr><th>Stat</th><td><!--gapless in--></td>"
          "<th width='%dpx'>Progress</th>"  // progress bar.
          "<td><!-- gapless out --></td>"
          "<th>Pos</th><td></td><th>Len</th><th>Max&nbsp;out</th><th>Format&nbsp;(used&nbsp;filter)</th>"
          "<th align='left'>File</th></tr>\n", kProgressWidth);
  CompareStats comparator;
  std::sort(stat_list.begin(), stat_list.end(), comparator);
  for (size_t i = 0; i < stat_list.size(); ++i) {
    AppendFileInfo(kActiveProgress, stat_list[i]);
  }
  content_.append("</table><hr/>\n");

  if (retired_.size() > 0) {
    content_.append("<h3>Retired</h3>\n");
    content_.append("<table>\n");
    boost::lock_guard<boost::mutex> l(retired_mutex_);
    for (RetiredList::const_iterator it = retired_.begin(); 
         it != retired_.end(); ++it) {
      AppendFileInfo(kRetiredProgress, *it);
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
          "<span style='float:right;font-size:x-small;'>"
          "&copy; 2012 Henner Zeller"
          " | Folve is free software and comes with no warranty. "
          " | Conveyed under the terms of the "
          "<a href='http://www.gnu.org/licenses/gpl.html'>GPLv3</a>.</span>"
          "</body></html>\n", duration * 1000.0);
  return content_;
}
