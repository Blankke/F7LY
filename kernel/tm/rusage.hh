/* Define struct rusage.
   Copyright (C) 1994-2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

#ifndef __rusage_defined
#define __rusage_defined 1

#include <sys/time.h>

/* Structure which says how much of each resource has been used.  If
   the system does not keep track of a particular value, the struct
   field is always zero.  */

/* The purpose of all the unions is to have the kernel-compatible layout
   while keeping the API type as 'long int', and among machines where
   long is not 'long int', this only does the right thing
   for little-endian ones, like x32.  */
struct rusage
  {
    /* Total amount of user time used.  */
    struct timeval ru_utime;
    /* Total amount of system time used.  */
    struct timeval ru_stime;
    /* Maximum resident set size (in kilobytes).  */
    __extension__ union
      {
	long int ru_maxrss;
	long __ru_maxrss_word;
      };
    /* Amount of sharing of text segment memory
       with other processes (kilobyte-seconds).  */
    __extension__ union
      {
	long int ru_ixrss;
	long __ru_ixrss_word;
      };
    /* Amount of data segment memory used (kilobyte-seconds).  */
    __extension__ union
      {
	long int ru_idrss;
	long __ru_idrss_word;
      };
    /* Amount of stack memory used (kilobyte-seconds).  */
    __extension__ union
      {
	long int ru_isrss;
	 long __ru_isrss_word;
      };
    /* Number of soft page faults (i.e. those serviced by reclaiming
       a page from the list of pages awaiting reallocation.  */
    __extension__ union
      {
	long int ru_minflt;
	long __ru_minflt_word;
      };
    /* Number of hard page faults (i.e. those that required I/O).  */
    __extension__ union
      {
	long int ru_majflt;
	long __ru_majflt_word;
      };
    /* Number of times a process was swapped out of physical memory.  */
    __extension__ union
      {
	long int ru_nswap;
	long __ru_nswap_word;
      };
    /* Number of input operations via the file system.  Note: This
       and `ru_oublock' do not include operations with the cache.  */
    __extension__ union
      {
	long int ru_inblock;
	long __ru_inblock_word;
      };
    /* Number of output operations via the file system.  */
    __extension__ union
      {
	long int ru_oublock;
	long __ru_oublock_word;
      };
    /* Number of IPC messages sent.  */
    __extension__ union
      {
	long int ru_msgsnd;
	long __ru_msgsnd_word;
      };
    /* Number of IPC messages received.  */
    __extension__ union
      {
	long int ru_msgrcv;
	long __ru_msgrcv_word;
      };
    /* Number of signals delivered.  */
    __extension__ union
      {
	long int ru_nsignals;
	long __ru_nsignals_word;
      };
    /* Number of voluntary context switches, i.e. because the process
       gave up the process before it had to (usually to wait for some
       resource to be available).  */
    __extension__ union
      {
	long int ru_nvcsw;
	long __ru_nvcsw_word;
      };
    /* Number of involuntary context switches, i.e. a higher priority process
       became runnable or the current process used up its time slice.  */
    __extension__ union
      {
	long int ru_nivcsw;
	long __ru_nivcsw_word;
      };
  };

#endif
