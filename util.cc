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

#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <sys/time.h>
#include <syslog.h>

#include <cstdarg>
#include <string.h>

double folve::CurrentTime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

static void vAppendf(std::string *str, const char *format, va_list ap) {
  const size_t orig_len = str->length();
  const size_t space = 1024;   // there should be better ways to do this...
  str->resize(orig_len + space);
  int written = vsnprintf((char*)str->data() + orig_len, space, format, ap);
  str->resize(orig_len + written);
}

void folve::Appendf(std::string *str, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  vAppendf(str, format, ap);
  va_end(ap);
}

std::string folve::StringPrintf(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  std::string result;
  vAppendf(&result, format, ap);
  va_end(ap);
  return result;
}

static bool global_debug_log = false;
void folve::EnableDebugLog(bool b) {
  if (b != global_debug_log) {
    syslog(LOG_INFO, "Switch debug mode %s.", b ? "on" : "off");
    global_debug_log = b;
  }
}

bool folve::IsDebugLogEnabled() {
  return global_debug_log;
}

void folve::DLogf(const char *format, ...) {
  if (!global_debug_log) return;
  va_list ap;
  va_start(ap, format);
  vsyslog(LOG_DEBUG, format, ap);
  va_end(ap);
}

bool folve::HasSuffix(const std::string &str, const std::string &suffix) {
  if (str.length() < suffix.length()) return false;
  return str.compare(str.length() - suffix.length(),
                     suffix.length(), suffix) == 0;
}

void *folve::Thread::PthreadCallRun(void *tobject) {
  reinterpret_cast<folve::Thread*>(tobject)->DoRun();
  return NULL;
}

folve::Thread::Thread() : lifecycle_(INIT) {
  pthread_cond_init(&lifecycle_condition_, NULL);
}
folve::Thread::~Thread() {
  WaitFinished();
  MutexLock l(&lifecycle_mutex_);
  if (lifecycle_ == STOPPED) {
    int result = pthread_join(thread_, NULL);
    if (result != 0) {
      fprintf(stderr, "err code: %d %s\n", result, strerror(result));
      assert(result == 0);
    }
    lifecycle_ = JOINED;  // just for good measure.
  }
}

void folve::Thread::DoRun() {
  Run();
  SetLifecycle(STOPPED);
}

bool folve::Thread::StartCalled() {   // more like: beyond init.
  MutexLock l(&lifecycle_mutex_);
  return lifecycle_ >= START_CALLED;
}

void folve::Thread::SetLifecycle(Lifecycle lifecycle) {
  MutexLock l(&lifecycle_mutex_);
  lifecycle_ = lifecycle;
  pthread_cond_signal(&lifecycle_condition_);
}

void folve::Thread::Start() {
  MutexLock l(&lifecycle_mutex_);
  assert(lifecycle_ == INIT);
  pthread_create(&thread_, NULL, &PthreadCallRun, this);
  lifecycle_ = START_CALLED;
}

void folve::Thread::WaitFinished() {
  MutexLock l(&lifecycle_mutex_);
  if (lifecycle_ == INIT) {
    lifecycle_ = JOINED;  // Never started ? Consider ourself joined.
    return;
  }
  if (lifecycle_ == STOPPED || lifecycle_ == JOINED) {
    return;
  }
  assert(lifecycle_ == START_CALLED);  // that is what we're in now.
  while (lifecycle_ != STOPPED) {
    lifecycle_mutex_.WaitOn(&lifecycle_condition_);
  }
}
