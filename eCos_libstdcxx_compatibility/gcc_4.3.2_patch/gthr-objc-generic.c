/* Threads compatibility routines for libobjc.  */ 
/* Compile this one with gcc.  */ 
/* Copyright (C) 1997, 1999, 2000, 2006 Free Software Foundation, Inc. 
 
This file is part of GCC. 
 
GCC is free software; you can redistribute it and/or modify it under 
the terms of the GNU General Public License as published by the Free 
Software Foundation; either version 2, or (at your option) any later 
version. 
 
GCC is distributed in the hope that it will be useful, but WITHOUT ANY 
WARRANTY; without even the implied warranty of MERCHANTABILITY or 
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License 
for more details. 
 
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
 
#include "tconfig.h" 
 
#define __GTHR_WEAK __attribute__ ((weak)) 
#define _LIBOBJC 
 
/* ??? The objc thread types are defined in ../libobjc/objc/thr.h, 
   but we don't want the gcc core to depend on libobjc.  */ 
typedef void * objc_thread_t; 
typedef struct objc_mutex *objc_mutex_t; 
typedef struct objc_condition *objc_condition_t; 
#define OBJC_THREAD_INTERACTIVE_PRIORITY        2 
 
#include "gthr.h" 
 
#define UNUSED(x) x ATTRIBUTE_UNUSED 
 
/* Just provide compatibility for mutex handling.  */ 
 
/* Thread local storage for a single thread */ 
static void *thread_local_storage = 0; 
 
/* Backend initialization functions */ 
 
/* Initialize the threads subsystem.  */ 
int 
__generic_gxx_objc_init_thread_system (void) 
{ 
  /* No thread support available */ 
  return -1; 
} 
 
/* Close the threads subsystem.  */ 
int 
__generic_gxx_objc_close_thread_system (void) 
{ 
  /* No thread support available */ 
  return -1; 
} 
 
/* Backend thread functions */ 
 
/* Create a new thread of execution.  The thread starts executing by calling 
   FUNC with ARG as its only argument. 
   On success, a handle for the new thread is returned. 
   On failure, zero is returned.  */ 
objc_thread_t 
__generic_gxx_objc_thread_detach (void UNUSED ((* func)(void *)), 
  void * UNUSED(arg)) 
{ 
  /* No thread support available */ 
  return 0; 
} 
 
/* Set the current thread's priority.  */ 
int 
__generic_gxx_objc_thread_set_priority (int UNUSED(priority)) 
{ 
  /* No thread support available */ 
  return -1; 
} 
 
/* Return the current thread's priority.  */ 
int 
__generic_gxx_objc_thread_get_priority (void) 
{ 
  return OBJC_THREAD_INTERACTIVE_PRIORITY; 
} 
 
/* Yield our process time to another thread.  */ 
void 
__generic_gxx_objc_thread_yield (void) 
{ 
  return; 
} 
 
/* Terminate the current thread.  */ 
int 
__generic_gxx_objc_thread_exit (void) 
{ 
  /* No thread support available */ 
  /* Should we really exit the program */ 
  /* exit (&__objc_thread_exit_status); */ 
  return -1; 
} 
 
/* Returns an integer value which uniquely describes a thread.  */ 
objc_thread_t 
__generic_gxx_objc_thread_id (void) 
{ 
  /* No thread support, use 1.  */ 
  return (objc_thread_t) 1; 
} 
 
/* Sets the thread's objc local storage pointer.  */ 
int 
__generic_gxx_objc_thread_set_data (void *value) 
{ 
  thread_local_storage = value; 
  return 0; 
} 
 
/* Returns the thread's objc local storage pointer.  */ 
void * 
__generic_gxx_objc_thread_get_data (void) 
{ 
  return thread_local_storage; 
} 
 
/* Backend mutex functions */ 
 
/* Allocate a backend-specific mutex data in MUTEX->backend.   
   Return 0 on success, -1 for failure.  */ 
int 
__generic_gxx_objc_mutex_allocate (objc_mutex_t UNUSED(mutex)) 
{ 
  return 0; 
} 
 
/* Deallocate backend-specific mutex data in MUTEX->backend. 
   Return 0 on success, -1 for failure.  */ 
int 
__generic_gxx_objc_mutex_deallocate (objc_mutex_t UNUSED(mutex)) 
{ 
  return 0; 
} 
 
/* Grab a lock on MUTEX.  Return 0 on success.  */ 
int 
__generic_gxx_objc_mutex_lock (objc_mutex_t UNUSED(mutex)) 
{ 
  /* There can only be one thread, so we always get the lock */ 
  return 0; 
} 
 
/* Try to grab a lock on MUTEX.  Return 0 on success.  */ 
int 
__generic_gxx_objc_mutex_trylock (objc_mutex_t UNUSED(mutex)) 
{ 
  /* There can only be one thread, so we always get the lock */ 
  return 0; 
} 
 
/* Unlock MUTEX.  Return 0 on success.  */ 
int 
__generic_gxx_objc_mutex_unlock (objc_mutex_t UNUSED(mutex)) 
{ 
  return 0; 
} 
 
/* Backend condition mutex functions */ 
 
/* Allocate backend-specific condition data in CONDITION->backend. 
   Return 0 on success, -1 for failure.  */ 
int 
__generic_gxx_objc_condition_allocate (objc_condition_t UNUSED(condition)) 
{ 
  return 0; 
} 
 
/* Deallocate backend-specific condition data in CONDITION->backend. 
   Return 0 for success.  */ 
int 
__generic_gxx_objc_condition_deallocate (objc_condition_t UNUSED(condition)) 
{ 
  return 0; 
} 
 
/* MUTEX is a locked mutex.  Atomically release MUTEX and wait on 
   CONDITION, i.e. so that no other thread can observe a state after 
   the release of MUTEX but before this thread has blocked. 
   Then re-acquire a lock on MUTEX. 
   Return 0 on success.  */ 
int 
__generic_gxx_objc_condition_wait (objc_condition_t UNUSED(condition), 
   objc_mutex_t UNUSED(mutex)) 
{ 
  return 0; 
} 
 
/* Wake up all threads waiting on CONDITION.  Return 0 on success.  */ 
int 
__generic_gxx_objc_condition_broadcast (objc_condition_t UNUSED(condition)) 
{ 
  return 0; 
} 
 
/* Wake up one thread waiting on CONDITION.  Return 0 on success.  */ 
int 
__generic_gxx_objc_condition_signal (objc_condition_t UNUSED(condition)) 
{ 
  return 0; 
} 
