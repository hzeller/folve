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

#include <algorithm>

#include "conversion-buffer.h"
#include "util.h"

BufferThread::BufferThread(int buffer_ahead)
  : buffer_ahead_size_(buffer_ahead), current_work_item_(NULL) {
  pthread_cond_init(&enqueue_event_, NULL);
  pthread_cond_init(&picked_work_, NULL);
}

void BufferThread::EnqueueWork(ConversionBuffer *buffer) {
  folve::MutexLock l(&mutex_);
  // We only need one element per buffer in the queue; when it is 'its' turn,
  // we'll calculate the right buffer horizon anyway. So don't accept more.
  //
  // This is O(n), but we only expect n in the order of ~4, the cache size.
  if (std::find(queue_.begin(), queue_.end(), buffer) != queue_.end()) {
    return;
  }
  queue_.push_back(buffer);
  pthread_cond_signal(&enqueue_event_);
}

void BufferThread::Forget(ConversionBuffer *buffer) {
  folve::MutexLock l(&mutex_);
  // Again, O(n), but typical n is low.
  WorkQueue::iterator it = queue_.begin();
  while (it != queue_.end()) {
    if (*it == buffer) {
      it = queue_.erase(it);
    } else  {
      ++it;
    }
  }
  // If this was currently what we were working on, wait until that is gone.
  while (current_work_item_ == buffer) {
    mutex_.WaitOn(&picked_work_);
  }
}

void BufferThread::Run() {
  for (;;) {
    {
      folve::MutexLock l(&mutex_);
      while (queue_.empty()) {
        mutex_.WaitOn(&enqueue_event_);
      }
      current_work_item_ = queue_.front();
      queue_.pop_front();
      pthread_cond_signal(&picked_work_);
    }

    ConversionBuffer *const buffer = current_work_item_;  // convenient name.
    const int kBufferChunk = (8 << 10);
    const off_t goal = buffer->MaxAccessed() + buffer_ahead_size_;
    while (!buffer->IsFileComplete() && buffer->FileSize() < goal) {
      // We do this in chunks so that the main thread has a chance to
      // get into there.
      buffer->FillUntil(buffer->FileSize() + kBufferChunk);
    }

    {
      folve::MutexLock l(&mutex_);
      current_work_item_ = NULL;
      pthread_cond_signal(&picked_work_);
    }
  }
}

