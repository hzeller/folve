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

// TODO: someone with a bit more stylesheet-fu can attempt to make this
// more pretty and the HTML more compact. Maybe even with some Ajax-y inline
// update of progress bars ?

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
  "  <div style='width:%d%%;background:%s;'>&nbsp;</div>\n</div></td>" \
  "<td>%s</td>"

#define sTimeColumns \
  "<td align='right'>%2d:%02d</td><td>/</td><td align='right'>%2d:%02d</td>"
#define sDecibelColumn \
  "<td align='right' style='background:%s;'>%.1f dB</td>"

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
    Appendf(result, sProgressRowHtml, status,
            stats.in_gapless ? "&rarr;" : "",
            kProgressWidth, (int) (100 * stats.progress), progress_style,
            stats.out_gapless ? "&rarr;" : "");
  }
  const int secs = stats.duration_seconds;
  const int fract_sec = stats.progress * secs;
  if (secs >= 0 && fract_sec >= 0) {
    Appendf(result, sTimeColumns, 
            fract_sec / 60, fract_sec % 60,
            secs / 60, secs % 60);
  } else {
    result->append("<td colspan='3'>-</td>");
  }

  if (stats.max_output_value > 1e-6) {
    Appendf(result, sDecibelColumn,
            stats.max_output_value > 1.0 ? "#FF0505" : "white",
            20 * log10f(stats.max_output_value));
  } else {
    result->append("<td>-</td>");
  }

  Appendf(result, "<td bgcolor='#c0c0c0'>&nbsp;%s&nbsp;</td>",
          stats.format.c_str());
  Appendf(result,"<td "
          "style='font-size:small;text-wrap:none;white-space:nowrap'>%s</td>",
          stats.filename.c_str());
  result->append("</tr>\n");
}

void StatusServer::PrepareConfigDirectoriesForUI() {
  // Essentially only keep the directory name.
  if (!ui_config_directories_.empty()) return;
  ui_config_directories_.push_back("[None : Pass Through]");
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
  result->append("<b>");
  for (size_t i = 0; i < options.size(); ++i) {
    const std::string &c = options[i];
    if (i != 0 ) result->append("&nbsp;|&nbsp;");
    const bool active = (int) i == selected;
    if (active) result->append("<span style='background:#a0a0ff'>");
    Appendf(result, "<input type='radio' onchange='this.form.submit();' "
            "id='f%zd' name='f' value='%zd'%s>"
            "<label for='f%zd'>%s</label>\n",
            i, i, active ? " checked='checked'" : "",
            i, c.c_str());
    if (active) result->append("</span>");
  }
  result->append("</b>");
}

void StatusServer::AppendSettingsForm() {
  Appendf(&content_, "<form id='settings-form' action='%s'>\n"
          "<label for='cfg_sel'>Config directory </label>", kSettingsUrl);
  PrepareConfigDirectoriesForUI();
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
  if (filesystem_->is_debug_ui_enabled()) {
    Appendf(&content_, "<span style='float:right;font-size:small;'>"
            "<label for='dbg_sel'>Folve debug to syslog</label>"
            "<input id='dbg_sel' onchange='this.form.submit();' "
            "type='checkbox' name='d' value='1'%s/></span>",
            folve::IsDebugLogEnabled() ? " checked" : "");
  }
  content_.append("</form>");
  content_.append("<script>document.forms['settings-form'].reset();</script>");
  content_.append("<hr/>");
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
          "<th>Pos</th><td></td><th>Len</th><th>Max out</th><th>Format</th>"
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
          "</body></html>\n", duration * 1000.0);
  return content_;
}
