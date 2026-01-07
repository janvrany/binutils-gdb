/* Copyright (C) 2025-2025 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef GDBSUPPORT_RANGE_H
#define GDBSUPPORT_RANGE_H

/* Return true if given ranges [AL, AH) and [BL, BH) overlap.  Return false
   otherwise.  */

template <typename T>
bool ranges_overlap (T al, T ah, T bl, T bh)
{
  static_assert (std::is_integral<T>::value, "Integral type required");

  gdb_assert (al <= ah);
  gdb_assert (bl <= bh);

  return !(ah <= bl || bh <= al);
}

#endif /* GDBSUPPORT_RANGE_H */
