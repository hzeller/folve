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
#ifndef FOLVE_BUFFER_THREAD_H_
#define FOLVE_BUFFER_THREAD_H_

#include "util.h"
#include <pthread.h>

#include <list>

class ConversionBuffer;
// NOTE: runs forever the whole program lifetime; does not provide a way to quit.
class BufferThread : public folve::Thread {
public:
  BufferThread(int buffer_ahead);

  // Enqueue a conversion buffer to work on.
  void EnqueueWork(ConversionBuffer *buffer);

  // If the given buffer is enqueued, forget about it. We don't need it anymore.
  void Forget(ConversionBuffer *buffer);

 protected:
  virtual void Run();

  const int buffer_ahead_size_;

  folve::Mutex mutex_;
  typedef std::list<ConversionBuffer *> WorkQueue;
  WorkQueue queue_;   // crude initial impl. of work-queue
  pthread_cond_t enqueue_event_;

  pthread_cond_t picked_work_;
  ConversionBuffer *current_work_item_;
};

#endif  // FOLVE_BUFFER_THREAD_H_
