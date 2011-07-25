/* Generic threads supplementary implementation. */ 
/* Compile this one with gcc.  */ 
/* Copyright (C) 1997, 1999, 2000, 2002, 2006 Free Software Foundation, Inc. 
 
This file is part of GCC. 
 
GCC is free software; you can redistribute it and/or modify 
it under the terms of the GNU General Public License as published by 
the Free Software Foundation; either version 2, or (at your option) 
any later version. 
 
GCC is distributed in the hope that it will be useful, 
but WITHOUT ANY WARRANTY; without even the implied warranty of 
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
GNU General Public License for more details. 
 
You should have received a copy of the GNU General Public License 
along with GCC; see the file COPYING.  If not, write to the Free 
Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 
02110-1301, USA.  */ 
 
/* As a special exception, if you link this library with other files, 
   some of which are compiled with GCC, to produce an executable, 
   this library does not by itself cause the resulting executable 
   to be covered by the GNU General Public License. 
   This exception does not however invalidate any other reasons why 
   the executable file might be covered by the GNU General Public License.  */ 
 
#define __GTHR_WEAK __attribute__ ((weak)) 
 
#include "tconfig.h" 
#include "gthr.h" 
 
#ifndef __gthr_generic_h 
#error "Generic thread support package not supported" 
#endif 
 
/* These are stub functions.  When threading is available, a suitable set of definitions should be linked in.  */ 
 
/* Return 1 if thread system is active, 0 if not.  */ 
int 
__generic_gxx_active_p (void) 
{ 
  return 0; 
} 
 
/* The following functions should return zero on success or the error 
   number.  If the operation is not supported, -1 is returned. 
 
   __generic_gxx_once 
   __generic_gxx_key_create 
   __generic_gxx_key_delete 
   __generic_gxx_setspecific 
   __generic_gxx_mutex_lock 
   __generic_gxx_mutex_trylock 
   __generic_gxx_mutex_unlock 
   __generic_gxx_recursive_mutex_lock 
   __generic_gxx_recursive_mutex_trylock 
   __generic_gxx_recursive_mutex_unlock  */ 
 
/* FUNC is a function that should be called without parameters. 
   *ONCE has been initialized to __GTHREAD_ONCE_INIT and is otherwise only 
   used in calls to __generic_gxx_once with FUNC as the second parameter. 
   If __generic_gxx_once succeeds, FUNC will have been called exactly once 
   since the initialization of ONCE through any number of calls of 
   __generic_gxx_once with this pair of ONCE and FUNC values.  */ 
int 
__generic_gxx_once (__gthread_once_t *once ATTRIBUTE_UNUSED, 
    void (*func)(void) ATTRIBUTE_UNUSED) 
{ 
  return -1; 
} 
 
/* Assign a key to *KEY that can be used in calls to 
   __generic_gxx_setspecific / __generic_gxx_getspecific. 
   If DTOR is nonzero, and at thread exit the value associated with the key 
   is nonzero, DTOR will be called at thread exit with the value associated 
   with the key as its only argument.  */ 
int 
__generic_gxx_key_create (__gthread_key_t *key ATTRIBUTE_UNUSED, 
  void (*dtor)(void *) ATTRIBUTE_UNUSED) 
{ 
  return -1; 
} 
 
/* KEY is a key previously allocated by __generic_gxx_key_create. 
   Remove it from the set of keys known for this thread.  */ 
int 
__generic_gxx_key_delete (__gthread_key_t key ATTRIBUTE_UNUSED) 
{ 
  return -1; 
} 
 
/* Return thread-specific data associated with KEY.  */ 
void * 
__generic_gxx_getspecific (__gthread_key_t key ATTRIBUTE_UNUSED) 
{ 
  return 0; 
} 
 
/* Set thread-specific data associated with KEY to PTR.  */ 
int 
__generic_gxx_setspecific (__gthread_key_t key ATTRIBUTE_UNUSED, 
      const void *ptr ATTRIBUTE_UNUSED) 
{ 
  return -1; 
} 
 
/* Initialize *MUTEX.  */ 
void 
__generic_gxx_mutex_init_function (__gthread_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
} 
 
/* Acquire a lock on *MUTEX.  The behaviour is undefined if a lock on *MUTEX 
   has already been acquired by the same thread.  */ 
int 
__generic_gxx_mutex_lock (__gthread_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
  return 0; 
} 
 
/* Try to acquire a lock on *MUTEX.  If a lock on *MUTEX already exists, 
   return an error code.  */ 
int 
__generic_gxx_mutex_trylock (__gthread_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
  return 0; 
} 
 
/* A lock on *MUTEX has previously been acquired with __generic_gxx_mutex_lock 
   or __generic_gxx_mutex_trylock.  Release the lock.  */ 
int 
__generic_gxx_mutex_unlock (__gthread_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
  return 0; 
} 
 
/* Initialize *MUTEX.  */ 
void 
__generic_gxx_recursive_mutex_init_function (__gthread_recursive_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
} 
 
/* Acquire a lock on *MUTEX.  If a lock on *MUTEX has already been acquired by 
   the same thread, succeed.  */ 
int 
__generic_gxx_recursive_mutex_lock (__gthread_recursive_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
  return 0; 
} 
 
/* Try to acquire a lock on *MUTEX.  If a lock on *MUTEX has already been 
   acquired by the same thread, succeed.  If any other lock on *MUTEX 
   already exists, return an error code.  */ 
int 
__generic_gxx_recursive_mutex_trylock (__gthread_recursive_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
  return 0; 
} 
 
/* A lock on *MUTEX has previously been acquired with 
   __generic_gxx_recursive_mutex_lock or 
   __generic_gxx_recursive_mutex_trylock.  Release the lock.  */ 
int 
__generic_gxx_recursive_mutex_unlock (__gthread_recursive_mutex_t *mutex ATTRIBUTE_UNUSED) 
{ 
  return 0; 
} 
