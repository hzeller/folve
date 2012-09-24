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
//  along with this program.  If not, see <http://www.gnu.org/licenses/>

#include <assert.h>
#include <stdio.h>

#include <map>
#include <vector>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>

#include "file-handler.h"
#include "file-handler-cache.h"
#include "util.h"

struct FileHandlerCache::Entry {
  Entry(FileHandler *h) : handler(h), references(0), last_access(0) {}
  FileHandler *const handler;
  int references;
  double last_access;  // seconds since epoch, sub-second resolution.
};

FileHandler *FileHandlerCache::InsertPinned(const std::string &key,
                                            FileHandler *handler) {
  boost::lock_guard<boost::mutex> l(mutex_);
  CacheMap::iterator ins
    = cache_.insert(std::make_pair(key, (Entry*)NULL)).first;
  if (ins->second == NULL) {
    ins->second = new Entry(handler);
  } else {
    delete handler;
  }
  ++ins->second->references;
  if (cache_.size() > max_size_) {
    CleanupOldestUnreferenced_Locked();
  }
  ins->second->last_access = folve::CurrentTime();
  if (observer_) observer_->InsertHandlerEvent(ins->second->handler);
  return ins->second->handler;
}

FileHandler *FileHandlerCache::FindAndPin(const std::string &key) {
  boost::lock_guard<boost::mutex> l(mutex_);
  CacheMap::iterator found = cache_.find(key);
  if (found == cache_.end())
    return NULL;
  ++found->second->references;
  found->second->last_access = folve::CurrentTime();
  return found->second->handler;
}

void FileHandlerCache::Unpin(const std::string &key) {
  boost::lock_guard<boost::mutex> l(mutex_);
  CacheMap::iterator found = cache_.find(key);
  assert(found != cache_.end());
  --found->second->references;
  // If we are already beyond cache size, clean up as soon as we get idle.
  if (found->second->references == 0 && cache_.size() > max_size_) {
    Erase_Locked(found);
  }
}

void FileHandlerCache::SetObserver(Observer *observer) {
  assert(observer_ == NULL);
  observer_ = observer;
}

void FileHandlerCache::GetStats(std::vector<HandlerStats> *stats) {
  HandlerStats s;
  boost::lock_guard<boost::mutex> l(mutex_);
  for (CacheMap::iterator it = cache_.begin(); it != cache_.end(); ++it) {
    it->second->handler->GetHandlerStatus(&s);
    s.status = ((it->second->references == 0)
                ? HandlerStats::IDLE
                : HandlerStats::OPEN);
    s.last_access = it->second->last_access;
    stats->push_back(s);
  }
}

void FileHandlerCache::Erase_Locked(CacheMap::iterator &cache_it) {
  if (observer_) observer_->RetireHandlerEvent(cache_it->second->handler);
  delete cache_it->second->handler;  // FileHandler
  delete cache_it->second;           // Entry
  cache_.erase(cache_it);
}

struct FileHandlerCache::CompareAge {
  bool operator() (const CacheMap::iterator &a, const CacheMap::iterator &b) {
    return a->second->last_access < b->second->last_access;
  }
};
void FileHandlerCache::CleanupOldestUnreferenced_Locked() {
  assert(cache_.size() > max_size_);  // otherwise we shouldn't have been called
  // While this iterating through the whole cache might look expensive,
  // in practice we're talking about 3 elements here.
  // If we had significantly more, e.g. broken clients that don't close files,
  // we need to keep better track of age.
  std::vector<CacheMap::iterator> for_removal;
  for (CacheMap::iterator it = cache_.begin(); it != cache_.end(); ++it) {
    if (it->second->references == 0) for_removal.push_back(it);
  }

  const size_t to_erase_count = std::min(cache_.size() - max_size_,
                                         for_removal.size());
  CompareAge comparator;
  std::sort(for_removal.begin(), for_removal.end(), comparator);
  for (size_t i = 0; i < to_erase_count; ++i) {
    Erase_Locked(for_removal[i]);
  }
}
  
