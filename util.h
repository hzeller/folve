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

#ifndef FOLVE_UTIL_H
#define FOLVE_UTIL_H

#include <string>
#include <pthread.h>

  // Define this with empty, if you're not using gcc.
#define PRINTF_FMT_CHECK(fmt_pos, args_pos) \
    __attribute__ ((format (printf, fmt_pos, args_pos)))

// Some utility functions needed everywhere.
namespace folve {
  // Returns the current time as seconds since the start of the unix epoch,
  // but in microsecond resolution.
  double CurrentTime();

  // Like snprintf, but print to a std::string instead.
  void Appendf(std::string *str, const char *format, ...) PRINTF_FMT_CHECK(2,3);

  // Convenience, that returns a string directly. A bit less efficient than
  // Appendf().
  std::string StringPrintf(const char *format, ...) PRINTF_FMT_CHECK(1, 2);

  // Return if "str" has suffix "suffix".
  bool HasSuffix(const std::string &str, const std::string &suffix);

  // Log formatted string if debugging enabled.
  void DLogf(const char *format, ...) PRINTF_FMT_CHECK(1, 2);
  void EnableDebugLog(bool b);
  bool IsDebugLogEnabled();

  // Importing boost::mutex posed too many dependencies on some embedded systems
  // with insufficient library support. So do our own barebone wrappers
  // around posix threads.

  // Non-recursive Mutex.
  class Mutex {
  public:
    Mutex() { pthread_mutex_init(&mutex_, NULL); }
    ~Mutex() { pthread_mutex_destroy(&mutex_); }
    void Lock() { pthread_mutex_lock(&mutex_); }
    void Unlock() { pthread_mutex_unlock(&mutex_); }
    void WaitOn(pthread_cond_t *cond) { pthread_cond_wait(cond, &mutex_); }

  private:
    pthread_mutex_t mutex_;
  };

  // Useful RAII wrapper around mutex.
  class MutexLock {
  public:
    MutexLock(Mutex *m) : mutex_(m) { mutex_->Lock(); }
    ~MutexLock() { mutex_->Unlock(); }
  private:
    Mutex *const mutex_;
  };

  // Thread.
  class Thread {
  public:
    Thread();
    virtual ~Thread();

    void Start();
    bool StartCalled();
    void WaitFinished();

    // Override this.
    virtual void Run() = 0;

  private:
    enum Lifecycle {
      INIT,
      START_CALLED,
      STOPPED,
      JOINED,
    };

    void DoRun();
    static void *PthreadCallRun(void *object);
    void SetLifecycle(Lifecycle l);

    pthread_cond_t lifecycle_condition_;
    Mutex lifecycle_mutex_;
    Lifecycle lifecycle_;

    pthread_t thread_;
  };
}  // namespece folve

#undef PRINTF_FMT_CHECK

#endif  // FOLVE_UTIL_H
