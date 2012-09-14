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

#ifndef _FUSE_CONVOLVER_FILE_HANDLER_CACHE_
#define _FUSE_CONVOLVER_FILE_HANDLER_CACHE_

#include <map>
#include <string>
#include <boost/thread/mutex.hpp>

class FileHandler;

// Cache of in-use file handlers. We sometimes get multiple requests for the
// same file, so we want to map them all to the same FileHandler.
//
// This Cache manages the lifecycle of a FileHandler object; the user creates
// it, but this Cache handles deletion.
// This container is thread-safe.
class FileHandlerCache {
public:
  FileHandlerCache(int max_size) : max_size_(max_size) {}

  // Insert a new object under the given key.
  // Ownership is handed over to this map.
  // If there was already an object stored under that key, that is returned
  // instead and the new object discarded.
  FileHandler *InsertPinned(const std::string &key, FileHandler *handler);
  
  // Find an object in this map and pin it down. You've to Unpin() it after
  // use.
  FileHandler *FindAndPin(const std::string &key);
  
  // Unpin object. If the last object is unpinned, the PinnedMap may decide
  // to delete it later (though typically will keep it around for a while).
  void Unpin(const std::string &key);
  
 private:
  struct Entry;
  typedef std::map<std::string, Entry*> CacheMap;

  void CleanupUnreferenced();

  const size_t max_size_;
  boost::mutex mutex_;
  CacheMap cache_;
};

#endif  // _FUSE_CONVOLVER_FILE_HANDLER_CACHE_