// -*- C++ -*-

/*

  Heap Layers: An Extensible Memory Allocation Infrastructure
  
  Copyright (C) 2000-2012 by Emery Berger
  http://www.cs.umass.edu/~emery
  emery@cs.umass.edu
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#ifndef HL_BUMPALLOC_H
#define HL_BUMPALLOC_H

/**
 * @class BumpAlloc
 * @brief Obtains memory in chunks and bumps a pointer through the chunks.
 * @author Emery Berger <http://www.cs.umass.edu/~emery>
 */

namespace HL {

  template <int ChunkSize,
	    class Super>
  class BumpAlloc : public Super {
  public:

    enum { Alignment = 1 };

    BumpAlloc (void)
      : _bump (NULL),
	_remaining (0)
    {}

    inline void * malloc (size_t sz) {
      // If there's not enough space left to fulfill this request, get
      // another chunk.
      if (_remaining < sz) {
	refill(sz);
      }
      char * old = _bump;
      _bump += sz;
      _remaining -= sz;
      return old;
    }

    /// Free is disabled (we only bump, never reclaim).
    inline bool free (void *) { return false; }

  private:

    /// The bump pointer.
    char * _bump;

    /// How much space remains in the current chunk.
    size_t _remaining;

    // Get another chunk.
    void refill (size_t sz) {
      if (sz < ChunkSize) {
	sz = ChunkSize;
      }
      _bump = (char *) Super::malloc (sz);
      _remaining = sz;
    }

  };

}

#endif
