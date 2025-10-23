/* This testcase is part of GDB, the GNU debugger.

   Copyright 2025-2025 Free Software Foundation, Inc.

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

int __attribute__ ((section (".text_give_me_one"))) __attribute__((noinline))
give_me_one ()
{
  return 1;
}

int __attribute__ ((section (".text_give_me_zero")))
give_me_zero ()
{
  return give_me_one() - 1;
}
