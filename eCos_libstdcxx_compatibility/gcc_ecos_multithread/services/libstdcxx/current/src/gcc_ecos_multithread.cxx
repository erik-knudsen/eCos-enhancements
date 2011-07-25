/*
 *	File: gcc_ecos_multithread.cxx
 *	Author: Erik Aagaard Knudsen
 *
 *	Revision History:
 *	17-may-2007: Original
 *
 *	Abstract:	libstdcxx compatibility routines for libstdcxx for eCos. These routi-
 *				nes should be used when gcc is built with enable-threads=generic,
*				--enable-libstdcxx-allocator=malloc and --enable-cstdio=stdio.
 *
 *	Platforms: eCos. 
 */
#include <cyg/infra/cyg_type.h>
#include <pkgconf/system.h>
#include <pkgconf/libstdcxx.h>

#if defined CYGPKG_LIBSTDCXX

#if (defined CYGIMP_SERVICES_LIBSTDCXX_SUPPORT)

#if (defined CYGIMP_SERVICES_LIBSTDCXX_DWARF2_UNWINDING)
//======================================================================================
/*Limitations: (ONLY RELEVANT FOR DWARF2 UNWINDING)
  It is required that __register_frame_info is called to support exception handling.
  __register_frame_info should be called before any c++ file level constructors using
  exception handling (try/catch) are activated. Likewise __deregister_frame_info should
  be called after all file level destructors using exception handling (try/catch) have
  been activated (when exiting - this is not really relevant for eCos).
  We need to assure that dynamic memory allocation and that libc is initialized before
  exceptions (and other libstdcxx functionality) are used. This is due to the way gcc
  is built (with --enable-libstdcxx-allocator=malloc and --enable-cstdio=stdio).
  This is accomplished here by wrapping up the calls of the two routines in
  a c++ class, and declaring an instantiation of the class at file level with a priority
  number a little higher than the CYG_INIT_LIBC priority number (higher number means lower
  priority).
  This means that class instantiations at file level with priority numbers higher than this
  can use DWARF2 exception handling in their constructors.
*/
#if (defined CYGIMP_SERVICES_LIBSTDCXX_THREADS_COMPATIBILITY_NONE || \
	 defined CYGIMP_SERVICES_LIBSTDCXX_THREADS_COMPATIBILITY_NATIVE)

//The object class for activating __register_frame_info and __deregister_frame_info.

typedef struct object_t
{
	//Reserve enough space for an EH frame initialization object.
	void *reserve[7];
} object_t;

externC unsigned char __EH_FRAME_BEGIN__[];
externC void __register_frame_info(void *, object_t *);
externC void __deregister_frame_info(void *);

class CObject
{
public:
    CObject()
	{
		__register_frame_info(__EH_FRAME_BEGIN__, &object);
	};

    ~CObject(void)
	{
		__deregister_frame_info(__EH_FRAME_BEGIN__);
	};

private:
    object_t object;
};

#define CYGBLD_OBJECT_INIT CYGBLD_ATTRIB_INIT_AFTER(CYG_INIT_LIBC)
static CObject object CYGBLD_OBJECT_INIT;

#endif
#endif


#if (defined CYGIMP_SERVICES_LIBSTDCXX_THREADS_COMPATIBILITY_NONE)

externC int __generic_gxx_active_p(void)
{
    return 0;
}

#endif


//GCC multithread support for native eCos threads.
//=====================================================================================
/*The functionality of supporting routines for native eCos threads are heavily inspired
  by eCos pthread routines.
*/
/*Limitations:
  It is not allowed to use cyg_thread_kill. Threads must exit voluntarely (libstdcxx uses
  malloc and malloc is used in this file also).
  There is a maximum for the number of keys that can be used and there is a maximum for
  the number of threads, which can use the support. These figures are configuration
  dependant.
*/
#if (defined CYGIMP_SERVICES_LIBSTDCXX_THREADS_COMPATIBILITY_NATIVE)

#include <cyg/kernel/kapi.h>
#include <stdlib.h>
#include <errno.h>
#include <cyg/hal/hal_arch.h>
#include <cyg/infra/cyg_ass.h>


//Internal mutex class for controlling access to shared data structures.
class Cethread_mutex
{
public:
    Cethread_mutex()
	{
		pmutex = &mutex;
		cyg_mutex_init(pmutex);
	};

    ~Cethread_mutex(void)
	{
	};

	int lock()
	{
		if (pmutex == NULL)
			return (EINVAL);

		cyg_bool_t st = cyg_mutex_lock(pmutex);

		return (st ? (0) : (EAGAIN));
	};

	int unlock()
	{
		if (pmutex == NULL)
			return (EINVAL);

		cyg_mutex_unlock(pmutex);

		return 0;
	};

private:
    cyg_mutex_t *pmutex;
	cyg_mutex_t mutex;
};

static Cethread_mutex ethread_mutex;


/* Avoid depedency on specific headers in the interface to gcc. 
   The general idea is that you dynamically allocate the required data 
   structures, and a void * is used to point to this dynamically allocated 
   data. If your implementation can put all the required information in
   the void * itself, that's fine too of course (this is the case here
   for __gthread_key_t and _gthread_once_t).
   libstdc++ inherits from the mutex types, which is why they need to be 
   wrapped up as structs.
*/ 
typedef void *__gthread_key_t; 
typedef void *__gthread_once_t; 
typedef struct __gthread_mutex_s { void *p; } __gthread_mutex_t; 
typedef struct __gthread_recursive_mutex_s { void *p; } __gthread_recursive_mutex_t; 




//Max. number of destructor iterations (similar to Pthread's PTHREAD_DESTRUCTOR_ITERATIONS).
#define ETHREAD_DESTRUCTOR_ITERATIONS CYGNUM_SERVICES_LIBSTDCXX_ETHREAD_DESTRUCTOR_ITERATIONS
//Max. number of possible keys.
#define ETHREAD_KEYS_MAX        CYGNUM_SERVICES_LIBSTDCXX_ETHREAD_KEYS_MAX
//Max. number of threads (+1) using the GCC exception mechanism.
#define ETHREAD_THREADS_MAX (CYGNUM_SERVICES_LIBSTDCXX_ETHREAD_THREADS_MAX + 1)

//Per-thread key allocation. The key map has a 1 bit set for each
//key that is free, zero if it is allocated.
#define KEY_MAP_TYPE cyg_uint32
#define KEY_MAP_TYPE_SIZE (sizeof(KEY_MAP_TYPE)*8) // in BITS!
static KEY_MAP_TYPE thread_key[ETHREAD_KEYS_MAX/KEY_MAP_TYPE_SIZE];
static void (*key_destructor[ETHREAD_KEYS_MAX]) (void *);

//Identification of per-thread data.
static cyg_ucount32 data_index = CYGNUM_KERNEL_THREADS_DATA_MAX + 1;
static void **thread_table[ETHREAD_THREADS_MAX];


//Thread destructor.
static void ethread_exit(CYG_ADDRWORD data)
{
	int table_index;
 	void **thread_data;

	//Get thread local data.
	table_index = (int)cyg_thread_get_data(data_index);
	thread_data = thread_table[table_index];
    if( thread_data != NULL )
    {
        //Call per-thread key destructors.
        cyg_bool destructors_called;
        int destructor_iterations = 0;
		int key;
        do
        {
            destructors_called = false;
        
            for(key = 0; key < ETHREAD_KEYS_MAX; key++)
            {
                //Skip unallocated keys
                if(thread_key[key/KEY_MAP_TYPE_SIZE] & 1<<(key%KEY_MAP_TYPE_SIZE))
                    continue;

                //Skip NULL destructors
                if(key_destructor[key] == NULL) continue;

                //Skip NULL data values
                if(thread_data[key] == NULL) continue;

                //If it passes all that, call the destructor.
                {
                    void* value = thread_data[key];
                    thread_data[key] = NULL;
                    key_destructor[key](value);
                }

                //Record that we called a destructor
                destructors_called = true;
            }

            //Count the iteration
            destructor_iterations++;
        
        } while(destructors_called &&
                 (destructor_iterations <= ETHREAD_DESTRUCTOR_ITERATIONS));

		ethread_mutex.lock();
		free(thread_table[table_index]);
		thread_table[table_index] = NULL;
		ethread_mutex.unlock();
    }
}


//Call init_routine just the once per control variable.
static int ethread_once(CYG_ADDRWORD *once_control,
                         void (*init_routine) (void))
{
    CYG_ADDRWORD old;

    // Do a test and set on the once_control object.
    ethread_mutex.lock();

    old = *once_control;
    *once_control = 1;

    ethread_mutex.unlock();

    // If the once_control was zero, call the init_routine().
    if(!old) init_routine();
    
    return 0;
}


//Create a key to identify a location in the thread specific data area.
//Each thread has its own distinct thread-specific data area but all are
//addressed by the same keys. The destructor function is called whenever a
//thread exits and the value associated with the key is non-NULL.
static int ethread_key_create(CYG_ADDRWORD *key,
                               void (*destructor) (void *))
{
	int i, k;

    ethread_mutex.lock();

	//First time initialization (for all threads).
	if (data_index > CYGNUM_KERNEL_THREADS_DATA_MAX)
	{
		int i;

		//Get thread data index.
		data_index = cyg_thread_new_data_index();   //Asserts if no available index.

		//Initialize the per-thread data key map.
		for(i = 0; i < (int)(ETHREAD_KEYS_MAX/KEY_MAP_TYPE_SIZE); i++)
			thread_key[i] = ~0;
	}

    k = -1;

    //Find a key to allocate
    for(i = 0; i < (int)(ETHREAD_KEYS_MAX/KEY_MAP_TYPE_SIZE); i++)
    {
        if (thread_key[i] != 0)
        {
            //We have a table slot with space available

            //Get index of ls set bit.
            HAL_LSBIT_INDEX(k, thread_key[i]);

            //Clear it
            thread_key[i] &= ~(1<<k);

            //Add index of word
            k += i * KEY_MAP_TYPE_SIZE;

            //Install destructor
            key_destructor[k] = destructor;
            
            //Break out with key found
            break;
        }
    }

    if(k != -1)
    {
		void **thread_data;

        //Plant a NULL in all the valid thread data slots for this
        //key in case we are reusing a key we used before.
		for (i = 1; i < ETHREAD_THREADS_MAX; i++)
			if ((thread_data = thread_table[i]) != NULL)
				thread_data[k] = NULL;
   }
    
    ethread_mutex.unlock();    

    if(k == -1) return (EAGAIN);

    *key = (CYG_ADDRWORD)k;
    
    return 0;
}


//Delete key.
static int ethread_key_delete(CYG_ADDRWORD key)
{
    ethread_mutex.lock();

    //Set the key bit to 1 to indicate it is free.
    thread_key[key/KEY_MAP_TYPE_SIZE] |= 1<<(key%(KEY_MAP_TYPE_SIZE));

    ethread_mutex.unlock();        
    
    return 0;
}


//Store the pointer value in the thread-specific data slot addressed
//by the key.
static int ethread_setspecific(CYG_ADDRWORD key, void *pointer)
{
	int table_index, i;
 	void **thread_data;

    if(thread_key[key/KEY_MAP_TYPE_SIZE] & 1<<(key%KEY_MAP_TYPE_SIZE))
        return (EINVAL);

	//Get thread local data.
	table_index = (int)cyg_thread_get_data(data_index);
	thread_data = thread_table[table_index];
	if (thread_data == NULL)
    {
		ethread_mutex.lock();

		for (table_index = 1; table_index < ETHREAD_THREADS_MAX; table_index++)
			if (thread_table[table_index] == NULL)
				break;
		if ((table_index == ETHREAD_THREADS_MAX) ||
			(!cyg_thread_add_destructor(&ethread_exit, 0)))
		{
			ethread_mutex.unlock();
			return (EINVAL);
		}

        //Allocate the per-thread data table
        thread_data = (void **)malloc(ETHREAD_KEYS_MAX * sizeof(void *));
		if (thread_data == NULL)
		{
			ethread_mutex.unlock();
			return (ENOMEM);
		}

		thread_table[table_index] = thread_data;

		ethread_mutex.unlock();
 
        //Clear out all entries
        for(i = 0; i < ETHREAD_KEYS_MAX; i++)
            thread_data[i] = NULL;

		cyg_thread_set_data(data_index, (CYG_ADDRWORD)table_index);
   }
    
    thread_data[key] = pointer;
    
    return 0;
}


//Retrieve the pointer value in the thread-specific data slot addressed
//by the key.
static void *ethread_getspecific(CYG_ADDRWORD key)
{
	int table_index;
 	void **thread_data;
    void *val;

    if(thread_key[key/KEY_MAP_TYPE_SIZE] & 1<<(key%KEY_MAP_TYPE_SIZE))
        return (NULL);

	//Get thread local data.
	table_index = (int)cyg_thread_get_data(data_index);
	thread_data = thread_table[table_index];

    if(thread_data == NULL)
        val = NULL;
    else val = thread_data[key];

    return (val);
}



externC int __generic_gxx_active_p(void)
{
    return 1;
}


externC int __generic_gxx_once(__gthread_once_t *once, void (*func) (void))
{
	return ethread_once((CYG_ADDRWORD *)once, func);
}


externC int __generic_gxx_key_create(__gthread_key_t *key, void (*dtor) (void *))
{
    return ethread_key_create((CYG_ADDRWORD *)key, dtor);
}


externC int __generic_gxx_key_delete(__gthread_key_t key)
{
    return ethread_key_delete((CYG_ADDRWORD)key);
}


externC void * __generic_gxx_getspecific(__gthread_key_t key)
{
    return ethread_getspecific((CYG_ADDRWORD)key);
}


externC int __generic_gxx_setspecific(__gthread_key_t key, void *ptr)
{
    return ethread_setspecific((CYG_ADDRWORD)key, ptr);
}


externC void __generic_gxx_mutex_init_function(__gthread_mutex_t *mutex)
{
	mutex->p = malloc(sizeof(cyg_mutex_t));
	CYG_ASSERT(mutex->p != 0, "Malloc out of space for gxx mutex.");

	cyg_mutex_init((cyg_mutex_t *)(mutex->p));
}


externC int __generic_gxx_mutex_lock(__gthread_mutex_t *mutex)
{
	if (mutex->p == NULL)
		return (EINVAL);

	cyg_bool_t st = cyg_mutex_lock((cyg_mutex_t *)(mutex->p));

	return (st ? (0) : (EAGAIN));
}


externC int __generic_gxx_mutex_trylock(__gthread_mutex_t *mutex)
{
	if (mutex->p == NULL)
		return (EINVAL);

	cyg_bool_t st = cyg_mutex_trylock((cyg_mutex_t *)(mutex->p));

	return (st ? (0) : (EAGAIN));
}


externC int __generic_gxx_mutex_unlock(__gthread_mutex_t *mutex)
{
	if (mutex->p == NULL)
		return (EINVAL);

	cyg_mutex_unlock((cyg_mutex_t *)(mutex->p));

	return 0;
}


typedef struct c_recursive_mutex_t
{
	cyg_mutex_t mutex;
	cyg_handle_t owner;
	cyg_uint32 count;
} c_recursive_mutex_t;


static void c_recursive_mutex_init_function(c_recursive_mutex_t *mx)
{
	mx->owner = (cyg_handle_t)0;
	mx->count = 0;
	cyg_mutex_init(&mx->mutex);
}


static int c_recursive_mutex_lock(c_recursive_mutex_t *mx)
{
	cyg_bool_t st = true;
	cyg_handle_t self = cyg_thread_self();

	if (self != mx->owner)
		st = cyg_mutex_lock(&mx->mutex);

	if (st)
	{
		mx->owner = self;
		mx->count++;
	}

	return ((st) ?  (0) : (EAGAIN));
}


static int c_recursive_mutex_trylock(c_recursive_mutex_t *mx)
{
	cyg_bool_t st = true;
	cyg_handle_t self = cyg_thread_self();

	if (self != mx->owner)
		st = cyg_mutex_trylock(&mx->mutex);

	if (st)
	{
		mx->owner = self;
		mx->count++;
	}

	return ((st) ? (0) : (EAGAIN));
}


static void c_recursive_mutex_unlock(c_recursive_mutex_t *mx)
{
	mx->count--;

	if (mx->count == 0)
	{
		mx->owner = (cyg_handle_t)0;
		cyg_mutex_unlock(&mx->mutex);
	}
}


externC void __generic_gxx_recursive_mutex_init_function(__gthread_recursive_mutex_t *mutex)
{
	mutex->p = malloc(sizeof(c_recursive_mutex_t));
	CYG_ASSERT(mutex->p != 0, "Malloc out of space for gxx recursive mutex.");

	c_recursive_mutex_init_function((c_recursive_mutex_t *)mutex->p);
}


externC int __generic_gxx_recursive_mutex_lock(__gthread_recursive_mutex_t *mutex)
{
	if (mutex->p == NULL)
		return (EINVAL);

	return c_recursive_mutex_lock((c_recursive_mutex_t *)(mutex->p));
}


externC int __generic_gxx_recursive_mutex_trylock(__gthread_recursive_mutex_t *mutex)
{
	if (mutex->p == NULL)
		return (EINVAL);

 	return c_recursive_mutex_trylock((c_recursive_mutex_t *)(mutex->p));
}


externC int __generic_gxx_recursive_mutex_unlock(__gthread_recursive_mutex_t *mutex)
{
	if (mutex->p == NULL)
		return (EINVAL);

	c_recursive_mutex_unlock((c_recursive_mutex_t *)(mutex->p));

	return 0;
}

#endif

#endif

#endif
