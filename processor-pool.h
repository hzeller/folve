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
#ifndef FOLVE_PROCESSOR_POOL_
#define FOLVE_PROCESSOR_POOL_

#include <map>
#include <deque>
#include "util.h"

class SoundProcessor;
class ProcessorPool {
public:
  ProcessorPool(int max_available);

  // Get a new SoundProcesor from this pool with the given configuration.
  SoundProcessor *GetOrCreate(const std::string &base_dir,
                              int sampling_rate, int channels, int bits);

  // Return a processor pack to the pool.
  void Return(SoundProcessor *processor);

private:
  typedef std::map<string, std::deque<SoundProcessor*> > PoolMap;
  folve::mutex pool_mutex_;
  PoolMap pool_;
};

#endif  // FOLVE_PROCESSOR_POOL_
