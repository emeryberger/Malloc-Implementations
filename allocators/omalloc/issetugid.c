/*
 * Copyright (C) - 2007 Robert Connolly
 *
 * Permission to reproduce, copy, delete, distribute, transmit, use, modify,
 * build upon or otherwise exploit this software, in any form, for any
 * purpose, in any way, and by anyone, including by methods that have not
 * yet been invented or conceived, is hereby granted.
 */

#include <unistd.h>

extern int __libc_enable_secure;

int issetugid(void)
{
  if (__libc_enable_secure)
    {
      return 1;
    }
  
  if (getuid() != geteuid())
    {
      return 1;
    }
  
  if (getgid() != getegid())
    {
      return 1;
    }
  
  /* Else */
  return 0;
}
