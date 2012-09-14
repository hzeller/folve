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
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>

#include "file-handler.h"
#include "file-handler-cache.h"

FileHandlerCache::Entry::Entry(const std::string &k, FileHandler *h)
  : key(k), handler(h) {}

FileHandler *FileHandlerCache::InsertPinned(const std::string &key,
                                            FileHandler *handler) {
  boost::lock_guard<boost::mutex> l(mutex_);
  CacheMap::iterator ins
    = cache_.insert(std::make_pair(key, (Entry*)NULL)).first;
  if (ins->second == NULL) {  // new entry
    ins->second = new Entry(key, handler);
  } else {
    delete handler;
  }
  ++ins->second->references;
  if (cache_.size() > max_size_) {
    CleanupUnreferenced();
  }
  return ins->second->handler;
}

FileHandler *FileHandlerCache::FindAndPin(const std::string &key) {
  boost::lock_guard<boost::mutex> l(mutex_);
  CacheMap::iterator found = cache_.find(key);
  if (found == cache_.end())
    return NULL;
  ++found->second->references;
  return found->second->handler;
}

void FileHandlerCache::Unpin(const std::string &key) {
  boost::lock_guard<boost::mutex> l(mutex_);
  CacheMap::iterator found = cache_.find(key);
  assert(found != cache_.end());
  --found->second->references;
}

void FileHandlerCache::GetStats(std::vector<const Entry *> *entries) {
  boost::lock_guard<boost::mutex> l(mutex_);
  for (CacheMap::iterator it = cache_.begin(); it != cache_.end(); ++it) {
    ++it->second->references;
    entries->push_back(it->second);
  }
}

void FileHandlerCache::CleanupUnreferenced() {
  for (CacheMap::iterator it = cache_.begin(); it != cache_.end(); ++it) {
    if (it->second->references == 0) {
      fprintf(stderr, "cleanup %s\n", it->first.c_str());
      delete it->second->handler;
      delete it->second;
      cache_.erase(it);
    }
  }
}
