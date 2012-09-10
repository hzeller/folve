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

// Simple interface to hook the (pure C) fuse-convolve code into some
// implementation of the filter (which we choose to do with C++).

#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

struct filter_object_t {};

// Create a new filter given the open filedescriptor and the path. Returns
// that filter in an opaque filter_object_t*
struct filter_object_t *create_filter(int filedes, const char *orig_path);

// Read from the given filter at the file-offset "offset, up to "size" bytes
// into  "buffer". Returns number of bytes read or a negative errno value.
int read_from_filter(struct filter_object_t *filter,
                     char *buffer, size_t size, off_t offset);

// At the end of the operation, close filter. Return 0 on success or negative
// errno value on failure.
int close_filter(struct filter_object_t *filter);

#ifdef __cplusplus
}  // extern "C"
#endif  /* __cplusplus */

