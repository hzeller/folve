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
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

// Microhttpd's headers (at least v0.4.4) are broken and don't include stdarg.h
#include <stdarg.h>
#include <microhttpd.h>

#include <algorithm>

#include "folve-filesystem.h"
#include "status-server.h"
#include "util.h"

using folve::Appendf;

// To be used in CSS constant.
#define PROGRESS_WIDTH "300"
static const int kProgressWidth = 300;

static const size_t kMaxRetired = 20;
static const char kActiveAccessProgress[]  = "#7070ff";
static const char kActiveBufferProgress[]  = "#bbffbb";
static const char kRetiredAccessProgress[] = "#d0d0e8";
static const char kRetiredBufferProgress[] = "#e0f0e0";
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
  "9GPwHJVuaFl3l4D1+h0UjIdbTh9SpP2KQ2AgSfVAdEQGx23tOopAAAAAElFTkSuQmCC'/>\n"
  "<meta http-equiv='Content-Type' content='text/html'; charset='utf-8'>\n";

// TODO: someone with a bit more stylesheet-fu can attempt to make this
// more pretty and the HTML more compact.
// Also, maybe move to external file, makes editing much faster, then compile
// that into a C-string that we can include it in the binary.
static const char kCSS[] =
  "<style type='text/css'>"
  " body { font-family:Sans-Serif; }\n"
  " a:link { text-decoration:none; }\n"
  " a:visited { text-decoration:none; }\n"
  " a:hover { text-decoration:underline; }\n"
  " a:active { text-decoration:underline; }\n"
  " .lbox { border:1px solid black; padding-right:2em; }\n" // legend box
  " .rounded_box, .filter_sel {\n"
  "        float: left;\n"
  "        margin: 5px;\n"
  "        margin-right: 5px;\n"
  "        margin-bottom: 5px;\n"
  "        padding: 5px 15px;\n"
  "        border-radius: 5px;\n"
  "        -moz-border-radius: 5px; }\n"
  " .filter_sel { font-weight:bold; }\n"
  " .active { background-color:#a0a0ff; }\n"
  " .inactive { background-color:#e0e0e0; }\n"
  " .inactive:hover { background-color:#e0e0ff;\n"
  "                   color: #000000;\n"
  "                   text-decoration:none;}\n"
  " .inactive:link { color: #000000;text-decoration:none;}\n"
  " .inactive:visited { color: #000000;text-decoration:none;}\n"
  // CSS classes used in the file-listin. Keep things compact.
  " td { text-wrap:none; white-space:nowrap; }\n"
  " .fn { font-size:small; text-wrap:none; white-space:nowrap; }\n"  // filename
  " .pf { width:" PROGRESS_WIDTH "px;\n"                   // progress frame
  "       background: white; border:1px solid black; }\n"
  " .nf { text-align:right; }\n"                           // number formatting.
  " .fb { background-color:#c0c0c0;"                       // format box
  "        border-radius: 3px;\n"
  "        -moz-border-radius: 3px; }\n"
  " .es { font-size:x-small; }\n"                           // extended status
  "</style>";

class StatusServer::HtmlFileHandler : public FileHandler {
public:
  HtmlFileHandler(StatusServer *server) : FileHandler("") {
    server->CreatePage(false, &content_);
    memset(&stat_, 0, sizeof(stat_));
    stat_.st_size = content_.length();
    stat_.st_mtime = time(NULL);
    stat_.st_nlink = 1;
    stat_.st_mode = 0100444;  // regular file. readonly.
  }

  virtual int Read(char *buf, size_t size, off_t offset) {
    int end = size + offset;
    if (end > (int) content_.length()) end = content_.length();
    if (offset >= end) return 0;
    memcpy(buf, content_.data() + offset, end - offset);
    return end - offset;
  }

  virtual int Stat(struct stat *st) {
    *st = stat_;
    return 0;
  }

  // Get handler status.
  virtual void GetHandlerStatus(HandlerStats *s) {}

private:
  std::string content_;
  struct stat stat_;
};

// Callback function called by micro http daemon. Gets the StatusServer pointer
// in the user_argument.
StatusServer::HandleHttpResult
StatusServer::HandleHttp(void* user_argument,
                         struct MHD_Connection *connection,
                         const char *url, const char *method,
                         const char *version,
                         const char *upload_data, size_t *upload_size,
                         void**) {
  StatusServer* server = (StatusServer*) user_argument;
  struct MHD_Response *response;
  HandleHttpResult ret;

  if (strcmp(url, kSettingsUrl) == 0) {
    server->SetFilter(MHD_lookup_connection_value(connection,
                                                  MHD_GET_ARGUMENT_KIND, "f"));
    // We redirect to slash after this, to remove parameters from the GET-URL
    response = MHD_create_response_from_buffer(0, (void*)"",
                                               MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(response, "Location", "/");
    ret = MHD_queue_response(connection, 302, response);
  } else {
    const std::string &page = server->CreateHttpPage();
    response = MHD_create_response_from_buffer(page.length(),
                                               (void*) page.data(),
                                               MHD_RESPMEM_MUST_COPY);
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
  : expunged_retired_(0),
    meta_refresh_time_(-1),
    filesystem_(fs), daemon_(NULL), filter_switched_(false) {
  fs->handler_cache()->SetObserver(this);
}

FileHandler *StatusServer::CreateStatusFileHandler() {
  return new HtmlFileHandler(this);
}

void StatusServer::SetFilter(const char *filter) {
  if (filter == NULL) return;
  filter_switched_ = filesystem_->SwitchCurrentConfigDir(filter);
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

bool StatusServer::show_details() {
  // Right now, we just make this dependent on debug output. Could be a separate
  // option.
  return folve::IsDebugLogEnabled();
}

// FileHandlerCache::Observer interface.
void StatusServer::RetireHandlerEvent(FileHandler *handler) {
  HandlerStats stats;
  handler->GetHandlerStatus(&stats);  // Get last available stats.
  stats.last_access = folve::CurrentTime();
  stats.status = HandlerStats::RETIRED;
  folve::MutexLock l(&retired_mutex_);
  retired_.push_front(stats);
  while (retired_.size() > kMaxRetired) {
    ++expunged_retired_;
    retired_.pop_back();
  }
}

// The directories are user-input, so we need to sanitize stuff.
static void AppendSanitizedUrlParam(const std::string &in, std::string *out) {
  for (std::string::const_iterator i = in.begin(); i != in.end(); ++i) {
    if (isupper(*i) || islower(*i) || isdigit(*i)) {
      out->append(1, *i);
    } else {
      Appendf(out, "%%%02x", (unsigned char) *i);
    }
  }
}

static void AppendSanitizedHTML(const std::string &in, std::string *out) {
  for (std::string::const_iterator i = in.begin(); i != in.end(); ++i) {
    switch (*i) {
    case '<': out->append("&lt;"); break;
    case '>': out->append("&gt;"); break;
    case '&': out->append("&amp;"); break;
    default: out->append(1, *i);
    }
  }
}

// As ugly #defines, so that gcc can warn about printf() format problems.
#define sMessageRowHtml \
  "<td>%s</td><td colspan='3' style='font-size:small;'>%s</td>"

#define sProgressRowHtml \
  "<td>%s</td>"  \
  "<td>%s</td>"  \
  "<td><div class='pf'>" \
  "<div style='width:%dpx;background:%s;float:left;'>&nbsp;</div>" \
  "<div style='width:%dpx;background:%s;float:left;'>&nbsp;</div>" \
  "<p style='clear:both;'></p>" \
  "</div>\n</td><td>%s</td>"

#define sTimeColumns \
  "<td class='nf'>%2d:%02d</td><td>/</td><td class='nf'>%2d:%02d</td>"
#define sDecibelColumn \
  "<td class='nf'%s>%.1f dB</td>"

void StatusServer::AppendFileInfo(const char *progress_access_color,
                                  const char *progress_buffer_color,
                                  const HandlerStats &stats,
                                  std::string *out) {
  out->append("<tr>");
  const char *status = "";
  switch (stats.status) {
  case HandlerStats::OPEN:    status = "open"; break;
  case HandlerStats::IDLE:    status = "idle"; break;
  case HandlerStats::RETIRED: status = "&nbsp;----&nbsp;"; break;
    // no default to let the compiler detect new values.
  }

  char extended_status[64];
  if (show_details()) {
    const double time_ago = folve::CurrentTime() - stats.last_access;
    snprintf(extended_status, sizeof(extended_status),
             "%s <span class='es'>(%1.1fs)</span>",
             status, time_ago);
    status = extended_status;
  }
  if (!stats.message.empty()) {
    Appendf(out, sMessageRowHtml, status, stats.message.c_str());
  } else if (stats.access_progress == 0 && stats.buffer_progress <= 0) {
    // TODO(hzeller): we really need a way to display message and progress
    // bar in parallel.
    Appendf(out, sMessageRowHtml, status, "Only header accessed");
  } else {
    float accessed = stats.access_progress;
    float buffered
      = stats.buffer_progress > accessed ? stats.buffer_progress - accessed : 0;
    Appendf(out, sProgressRowHtml, status,
            stats.in_gapless ? "&rarr;" : "",
            (int) (kProgressWidth * accessed), progress_access_color,
            (int) (kProgressWidth * buffered), progress_buffer_color,
            stats.out_gapless ? "&rarr;" : "");
  }
  const int secs = stats.duration_seconds;
  const int fract_sec = stats.access_progress * secs;
  if (secs >= 0 && fract_sec >= 0) {
    Appendf(out, sTimeColumns,
            fract_sec / 60, fract_sec % 60,
            secs / 60, secs % 60);
  } else {
    out->append("<td colspan='3'>-</td>");
  }

  if (stats.max_output_value > 1e-6) {
    Appendf(out, sDecibelColumn,
            stats.max_output_value > 1.0 ? " style='background:#FF8080;'" : "",
            20 * log10f(stats.max_output_value));
  } else {
    out->append("<td>-</td>");
  }

  const char *filter_dir = stats.filter_dir.empty()
    ? "Pass Through" : stats.filter_dir.c_str();
  Appendf(out, "<td class='fb'>&nbsp;%s (", stats.format.c_str());
  AppendSanitizedHTML(filter_dir, out);
  out->append(")&nbsp;</td><td class='fn'>");
  AppendSanitizedHTML(stats.filename, out);
  out->append("</td></tr>\n");
}

static void CreateSelection(bool for_http,
                            const std::set<std::string> &options,
                            const std::string &selected,
                            std::string *result) {
  if (options.size() == 1) {
    result->append(selected);
    return;
  }
  typedef std::set<std::string> Set;
  for (Set::const_iterator it = options.begin(); it != options.end(); ++it) {
    const bool active = (*it == selected);
    const char *title = it->empty() ? "None : Pass Through" : it->c_str();
    if (active) {
      result->append("<span class='filter_sel active'>");
      AppendSanitizedHTML(title, result);
      result->append("</span>");
    } else if (for_http) { // only provide links if we have this in http.
      Appendf(result, "<a class='filter_sel inactive' href='%s?f=",
              kSettingsUrl);
      AppendSanitizedUrlParam(*it, result);
      result->append("'>");
      AppendSanitizedHTML(title, result);
      result->append("</a>\n");
    }
  }
}

void StatusServer::AppendSettingsForm(bool for_http, std::string *out) {
  out->append("<p><span class='filter_sel'>Active filter:</span>");
  std::set<std::string> available_dirs = filesystem_->GetAvailableConfigDirs();
  CreateSelection(for_http,
                  available_dirs, filesystem_->current_config_subdir(), out);
  if (available_dirs.empty() == 1) {
    out->append(" (This is a boring configuration, add filter directories)");
  } else if (filter_switched_) {
    out->append("<span class='rounded_box' "
                "style='font-size:small;background:#FFFFa0;'>"
                "Affects re- or newly opened files.</span>");
    filter_switched_ = false;  // only show once.
  }
  out->append("</p>");
}

struct CompareStats {
  bool operator() (const HandlerStats *a, const HandlerStats *b) {
    if (a->status < b->status) return true;   // open before idle.
    else if (a->status > b->status) return false;
    return b->last_access < a->last_access;   // reverse time.
  }
};


const std::string &StatusServer::CreateHttpPage() {
  CreatePage(true, &http_content_);
  return http_content_;
}

void StatusServer::CreatePage(bool for_http, std::string *content) {
  const double start = folve::CurrentTime();
  // We re-use a string to avoid re-allocing memory every time we generate
  // a page. Since we run with MHD_USE_SELECT_INTERNALLY, this is only accessed
  // by one thread.
  content->clear();
  content->append(kStartHtmlHeader);
  if (for_http && meta_refresh_time_ > 0) {
    Appendf(content, "<meta http-equiv='refresh' content='%d'>\n",
            meta_refresh_time_);
  }
  content->append(kCSS);
  content->append("</head>\n");

  content->append("<body>\n");
  Appendf(content, "<center style='background-color:#A0FFA0;'>"
          "Welcome to "
          "<a href='https://github.com/hzeller/folve#readme'>Folve</a> "
          FOLVE_VERSION "</center>\n");
  if (show_details()) {
    Appendf(content, "Convolving audio files from <code>%s</code>; "
            "Filter base directory <code>%s</code>\n",
            filesystem_->underlying_dir().c_str(),
            filesystem_->base_config_dir().c_str());
  }

  if (!filesystem_->toplevel_directory_is_filter()) {
      // Only show settings form if we are not representing the toplevel
      // directories as filters.
      AppendSettingsForm(for_http, content);
  }
  content->append("<hr style='clear:both;'/>");

  std::vector<HandlerStats> stat_list;
  filesystem_->handler_cache()->GetStats(&stat_list);

  if (show_details()) {
    Appendf(content, "Total opening files <b>%d</b> "
	    ".. and re-opened from recency cache <b>%d</b><br/>",
	    filesystem_->total_file_openings(),
	    filesystem_->total_file_reopen());
  }

  content->append("<h3>Accessed Recently</h3>\n");

  if (filesystem_->pre_buffer_size() > 0) {
    Appendf(content,
            "Accessed <span class='lbox' style='background:%s;'>&nbsp;</span> "
            "&nbsp; &nbsp; Predictive Buffer "
            "<span class='lbox' style='background:%s;'>&nbsp;</span>"
            " &nbsp; &nbsp; ",
            kActiveAccessProgress, kActiveBufferProgress);
  }
  if (filesystem_->gapless_processing()) {
    content->append("Gapless transfers indicated with '&rarr;'\n");
  }
  content->append("<table>\n");
  Appendf(content, "<tr><th>Stat%s</th><td><!--gapless in--></td>"
          "<th width='%dpx'>Progress</th>"  // progress bar.
          "<td><!-- gapless out --></td>"
          "<th>Pos</th><td></td><th>Len</th><th>Max&nbsp;out</th>"
          "<th>Format&nbsp;(used&nbsp;filter)</th>"
          "<th align='left'>File</th></tr>\n",
          show_details() ? " <span class='es'>(last)</span>" : "",
          kProgressWidth);
  // ICE in arm 2.7.1 compiler if we sort values. So sort pointers
  // to the values instead.
  std::vector<HandlerStats *> stat_ptrs;
  for (size_t i = 0; i < stat_list.size(); ++i) {
    stat_ptrs.push_back(&stat_list[i]);
  }
  CompareStats comparator;
  std::sort(stat_ptrs.begin(), stat_ptrs.end(), comparator);
  for (size_t i = 0; i < stat_ptrs.size(); ++i) {
    AppendFileInfo(kActiveAccessProgress, kActiveBufferProgress, *stat_ptrs[i],
                   content);
  }
  content->append("</table><hr/>\n");

  if (retired_.size() > 0) {
    content->append("<h3>Retired</h3>\n");
    content->append("<table>\n");
    folve::MutexLock l(&retired_mutex_);
    for (RetiredList::const_iterator it = retired_.begin();
         it != retired_.end(); ++it) {
      AppendFileInfo(kRetiredAccessProgress, kRetiredBufferProgress, *it,
                     content);
    }
    content->append("</table>\n");
    if (expunged_retired_ > 0) {
      Appendf(content, "... (%d more)<p></p>", expunged_retired_);
    }
    content->append("<hr/>");
  }

  const double duration = folve::CurrentTime() - start;
  Appendf(content,
          "<span style='float:left;font-size:x-small;'>%.2fms</span>"
          "<span style='float:right;font-size:x-small;'>"
          "&copy; 2012 Henner Zeller"
          " | Folve is free software and comes with no warranty. "
          " | Conveyed under the terms of the "
          "<a href='http://www.gnu.org/licenses/gpl.html'>GPLv3</a>.</span>"
          "</body></html>\n", duration * 1000.0);
}
