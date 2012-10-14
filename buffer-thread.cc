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

#include "buffer-thread.h"

#include <assert.h>
#include <pthread.h>
#include <algorithm>

#include "conversion-buffer.h"
#include "util.h"

BufferThread::BufferThread(int buffer_ahead)
  : buffer_ahead_size_(buffer_ahead), current_work_buffer_(NULL) {
  pthread_cond_init(&enqueue_event_, NULL);
  pthread_cond_init(&picked_work_, NULL);
}

void BufferThread::EnqueueWork(ConversionBuffer *buffer) {
  const off_t goal = buffer->MaxAccessed() + buffer_ahead_size_;
  folve::MutexLock l(&mutex_);
  bool found = false;
  // This is O(n), but n is typically in the order of max=4
  for (WorkQueue::iterator it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->buffer == buffer) {
      it->goal = goal;  // Already in queue; update goal.
      found = true;
      break;
    }
  }
  if (!found) {
    WorkItem new_work;
    new_work.buffer = buffer;
    new_work.goal = goal;
    queue_.push_back(new_work);
    pthread_cond_signal(&enqueue_event_);
  }
}

void BufferThread::Forget(ConversionBuffer *buffer) {
  folve::MutexLock l(&mutex_);
  // If this was currently what we were working on, wait until that is gone
  // to not delete conversion buffer being accessed.
  while (current_work_buffer_ == buffer) {
    mutex_.WaitOn(&picked_work_);
  }

  // Again, O(n), but typical n is low.
  WorkQueue::iterator it = queue_.begin();
  while (it != queue_.end()) {
    if (it->buffer == buffer) {
      it = queue_.erase(it);
    } else  {
      ++it;
    }
  }
}

void BufferThread::Run() {
  const int kBufferChunk = (8 << 10);
  for (;;) {
    WorkItem work;
    {
      folve::MutexLock l(&mutex_);
      while (queue_.empty()) {
        mutex_.WaitOn(&enqueue_event_);
      }
      work = queue_.front();
      current_work_buffer_ = work.buffer;
      pthread_cond_signal(&picked_work_);
    }

    // We only do one chunk at the time so that the main thread has a chance to
    // get into there and _we_ can round-robin through all work scheduled.
    const bool work_complete
      = (work.buffer->FillUntil(work.buffer->FileSize() + kBufferChunk)
         || work.buffer->FileSize() >= work.goal);

    {
      folve::MutexLock l(&mutex_);
      assert(queue_.front().buffer == current_work_buffer_);
      if (!work_complete) { // More work to do ? Re-schedule.
        queue_.push_back(queue_.front());
      }
      queue_.pop_front();
      current_work_buffer_ = NULL;
      pthread_cond_signal(&picked_work_);
    }
    pthread_yield();
  }
}

