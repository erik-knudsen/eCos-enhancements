/* Generic threads compatibility routines for libgcc2 and libobjc. */ 
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
 
#ifndef __gthr_generic_h 
#define __gthr_generic_h 
 
#define __GTHREADS 1 
 
#define __GTHREAD_ONCE_INIT 0 
#define __GTHREAD_MUTEX_INIT_FUNCTION __gthread_mutex_init_function 
#define __GTHREAD_RECURSIVE_MUTEX_INIT_FUNCTION __gthread_recursive_mutex_init_function 
 
#ifdef __cplusplus 
extern "C" { 
#endif 
 
/* Avoid depedency on specific headers. 
   The general idea is that you dynamically allocate the required data 
   structures, and a void * is used to point to this dynamically allocated 
   data.  If your implementation can put all the required information in 
   the void * itself, that's fine, too, of course. 
   libstdc++ inherits from the mutex types, which is why they need to be 
   wrapped up as structs.  */ 
typedef void *__gthread_key_t; 
typedef void *__gthread_once_t; 
typedef struct __gthread_mutex_s { void *p; } __gthread_mutex_t; 
typedef struct __gthread_recursive_mutex_s { void *p; } __gthread_recursive_mutex_t; 
 
/* We should always link with at least one definition, so we want strong 
   references.  The stub definitions are weak so that they can be overriden.  */ 
#ifndef __GTHR_WEAK 
#define __GTHR_WEAK 
#endif 
 
extern int __generic_gxx_active_p (void) __GTHR_WEAK; 
 
extern int __generic_gxx_once (__gthread_once_t *, void (*)(void)) __GTHR_WEAK; 
 
extern int __generic_gxx_key_create (__gthread_key_t *, 
     void (*)(void *)) __GTHR_WEAK; 
 
extern int __generic_gxx_key_delete (__gthread_key_t key) __GTHR_WEAK; 
 
extern void *__generic_gxx_getspecific (__gthread_key_t key) __GTHR_WEAK; 
 
extern int __generic_gxx_setspecific (__gthread_key_t, const void *) __GTHR_WEAK; 
 
extern void __generic_gxx_mutex_init_function (__gthread_mutex_t *) __GTHR_WEAK; 
 
extern int __generic_gxx_mutex_lock (__gthread_mutex_t *) __GTHR_WEAK; 
 
extern int __generic_gxx_mutex_trylock (__gthread_mutex_t *) __GTHR_WEAK; 
 
extern int __generic_gxx_mutex_unlock (__gthread_mutex_t *) __GTHR_WEAK; 
 
extern void __generic_gxx_recursive_mutex_init_function (__gthread_recursive_mutex_t *) __GTHR_WEAK; 
 
extern int __generic_gxx_recursive_mutex_lock (__gthread_recursive_mutex_t *) __GTHR_WEAK; 
 
extern int __generic_gxx_recursive_mutex_trylock (__gthread_recursive_mutex_t *) __GTHR_WEAK; 
 
extern int __generic_gxx_recursive_mutex_unlock (__gthread_recursive_mutex_t *) __GTHR_WEAK; 
 
#ifdef __cplusplus 
} 
#endif 
 
#ifdef _LIBOBJC 
 
extern int __generic_gxx_objc_init_thread_system (void) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_close_thread_system (void) __GTHR_WEAK; 
 
extern objc_thread_t __generic_gxx_objc_thread_detach (void (*)(void *), void *) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_thread_set_priority (int priority) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_thread_get_priority (void) __GTHR_WEAK; 
 
extern void __generic_gxx_objc_thread_yield (void) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_thread_exit (void) __GTHR_WEAK; 
 
extern objc_thread_t __generic_gxx_objc_thread_id (void) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_thread_set_data (void *value) __GTHR_WEAK; 
 
extern void *__generic_gxx_objc_thread_get_data (void) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_mutex_allocate (objc_mutex_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_mutex_deallocate (objc_mutex_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_mutex_lock (objc_mutex_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_mutex_trylock (objc_mutex_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_mutex_unlock (objc_mutex_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_condition_allocate (objc_condition_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_condition_deallocate (objc_condition_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_condition_wait (objc_condition_t, objc_mutex_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_condition_broadcast (objc_condition_t) __GTHR_WEAK; 
 
extern int __generic_gxx_objc_condition_signal (objc_condition_t) __GTHR_WEAK; 
 
/* Backend initialization functions */ 
 
/* Initialize the threads subsystem.  */ 
static inline int 
__gthread_objc_init_thread_system (void) 
{ 
  return __generic_gxx_objc_init_thread_system (); 
} 
 
/* Close the threads subsystem.  */ 
static inline int 
__gthread_objc_close_thread_system (void) 
{ 
  return __generic_gxx_objc_close_thread_system (); 
} 
 
/* Backend thread functions */ 
 
/* Create a new thread of execution.  */ 
static inline objc_thread_t 
__gthread_objc_thread_detach (void (* func)(void *), void * arg) 
{ 
  return __generic_gxx_objc_thread_detach (func, arg); 
} 
 
/* Set the current thread's priority.  */ 
static inline int 
__gthread_objc_thread_set_priority (int priority) 
{ 
  return __generic_gxx_objc_thread_set_priority (priority); 
} 
 
/* Return the current thread's priority.  */ 
static inline int 
__gthread_objc_thread_get_priority (void) 
{ 
  return __generic_gxx_objc_thread_get_priority (); 
} 
 
/* Yield our process time to another thread.  */ 
static inline void 
__gthread_objc_thread_yield (void) 
{ 
  __generic_gxx_objc_thread_yield (); 
} 
 
/* Terminate the current thread.  */ 
static inline int 
__gthread_objc_thread_exit (void) 
{ 
  return __generic_gxx_objc_thread_exit (); 
} 
 
/* Returns an integer value which uniquely describes a thread.  */ 
static inline objc_thread_t 
__gthread_objc_thread_id (void) 
{ 
  return __generic_gxx_objc_thread_id (); 
} 
 
/* Sets the thread's local storage pointer.  */ 
static inline int 
__gthread_objc_thread_set_data (void *value) 
{ 
  return __generic_gxx_objc_thread_set_data (value); 
} 
 
/* Returns the thread's local storage pointer.  */ 
static inline void * 
__gthread_objc_thread_get_data (void) 
{ 
  return __generic_gxx_objc_thread_get_data (); 
} 
 
/* Backend mutex functions */ 
 
/* Allocate a mutex.  */ 
static inline int 
__gthread_objc_mutex_allocate (objc_mutex_t mutex) 
{ 
  return __generic_gxx_objc_mutex_allocate (mutex); 
} 
 
/* Deallocate a mutex.  */ 
static inline int 
__gthread_objc_mutex_deallocate (objc_mutex_t mutex) 
{ 
  return __generic_gxx_objc_mutex_deallocate (mutex); 
} 
 
/* Grab a lock on a mutex.  */ 
static inline int 
__gthread_objc_mutex_lock (objc_mutex_t mutex) 
{ 
  return __generic_gxx_objc_mutex_lock (mutex); 
} 
 
/* Try to grab a lock on a mutex.  */ 
static inline int 
__gthread_objc_mutex_trylock (objc_mutex_t mutex) 
{ 
  return __generic_gxx_objc_mutex_trylock (mutex); 
} 
 
/* Unlock the mutex */ 
static inline int 
__gthread_objc_mutex_unlock (objc_mutex_t mutex) 
{ 
  return __generic_gxx_objc_mutex_unlock (mutex); 
} 
 
/* Backend condition mutex functions */ 
 
/* Allocate a condition.  */ 
static inline int 
__gthread_objc_condition_allocate (objc_condition_t condition) 
{ 
  return __generic_gxx_objc_condition_allocate (condition); 
} 
 
/* Deallocate a condition.  */ 
static inline int 
__gthread_objc_condition_deallocate (objc_condition_t condition) 
{ 
  return __generic_gxx_objc_condition_deallocate (condition); 
} 
 
/* Wait on the condition */ 
static inline int 
__gthread_objc_condition_wait (objc_condition_t condition, objc_mutex_t mutex) 
{ 
  return __generic_gxx_objc_condition_wait (condition, mutex); 
} 
 
/* Wake up all threads waiting on this condition.  */ 
static inline int 
__gthread_objc_condition_broadcast (objc_condition_t condition) 
{ 
  return __generic_gxx_objc_condition_broadcast ( condition); 
} 
 
/* Wake up one thread waiting on this condition.  */ 
static inline int 
__gthread_objc_condition_signal (objc_condition_t condition) 
{ 
  return __generic_gxx_objc_condition_signal (condition); 
} 
 
#else /* !_LIBOBJC */ 
 
static inline int 
__gthread_active_p (void) 
{ 
  return __generic_gxx_active_p (); 
} 
 
static inline int 
__gthread_once (__gthread_once_t *once, void (*func)(void)) 
{ 
  return __generic_gxx_once (once, func); 
} 
 
static inline int 
__gthread_key_create (__gthread_key_t *key, void (*dtor)(void *)) 
{ 
  return __generic_gxx_key_create (key, dtor); 
} 
 
static inline int 
__gthread_key_delete (__gthread_key_t key) 
{ 
  return __generic_gxx_key_delete (key); 
} 
 
static inline void * 
__gthread_getspecific (__gthread_key_t key) 
{ 
  return __generic_gxx_getspecific (key); 
} 
 
static inline int 
__gthread_setspecific (__gthread_key_t key, const void *ptr) 
{ 
  return __generic_gxx_setspecific (key, ptr); 
} 
 
static inline void 
__gthread_mutex_init_function (__gthread_mutex_t *mutex) 
{ 
  __generic_gxx_mutex_init_function (mutex); 
} 
 
static inline int 
__gthread_mutex_lock (__gthread_mutex_t * mutex) 
{ 
  return __generic_gxx_mutex_lock (mutex); 
} 
 
static inline int 
__gthread_mutex_trylock (__gthread_mutex_t * mutex) 
{ 
  return __generic_gxx_mutex_trylock (mutex); 
} 
 
static inline int 
__gthread_mutex_unlock (__gthread_mutex_t * mutex) 
{ 
  return __generic_gxx_mutex_unlock (mutex); 
} 
 
static inline void 
__gthread_recursive_mutex_init_function (__gthread_recursive_mutex_t *mutex) 
{ 
  __generic_gxx_recursive_mutex_init_function (mutex); 
} 
 
static inline int 
__gthread_recursive_mutex_lock (__gthread_recursive_mutex_t * mutex) 
{ 
  return __generic_gxx_recursive_mutex_lock (mutex); 
} 
 
static inline int 
__gthread_recursive_mutex_trylock (__gthread_recursive_mutex_t * mutex) 
{ 
  return __generic_gxx_recursive_mutex_trylock (mutex); 
} 
 
static inline int 
__gthread_recursive_mutex_unlock (__gthread_recursive_mutex_t * mutex) 
{ 
  return __generic_gxx_recursive_mutex_unlock (mutex); 
} 
 
#endif /* _LIBOBJC */ 

#endif /* __gthr_generic_h */ 
