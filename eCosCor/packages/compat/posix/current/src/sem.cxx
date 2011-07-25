//==========================================================================
//
//      sem.cxx
//
//      POSIX semaphore implementation
//
//==========================================================================
// ####ECOSGPLCOPYRIGHTBEGIN####                                            
// -------------------------------------------                              
// This file is part of eCos, the Embedded Configurable Operating System.   
// Copyright (C) 1998, 1999, 2000, 2001, 2002 Free Software Foundation, Inc.
//
// eCos is free software; you can redistribute it and/or modify it under    
// the terms of the GNU General Public License as published by the Free     
// Software Foundation; either version 2 or (at your option) any later      
// version.                                                                 
//
// eCos is distributed in the hope that it will be useful, but WITHOUT      
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or    
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License    
// for more details.                                                        
//
// You should have received a copy of the GNU General Public License        
// along with eCos; if not, write to the Free Software Foundation, Inc.,    
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.            
//
// As a special exception, if other files instantiate templates or use      
// macros or inline functions from this file, or you compile this file      
// and link it with other works to produce a work based on this file,       
// this file does not by itself cause the resulting work to be covered by   
// the GNU General Public License. However the source code for this file    
// must still be made available in accordance with section (3) of the GNU   
// General Public License v2.                                               
//
// This exception does not invalidate any other reasons why a work based    
// on this file might be covered by the GNU General Public License.         
// -------------------------------------------                              
// ####ECOSGPLCOPYRIGHTEND####                                              
//==========================================================================
//#####DESCRIPTIONBEGIN####
//
// Author(s):           nickg
// Contributors:        nickg
// Date:                2000-03-27
// Purpose:             POSIX semaphore implementation
// Description:         This file contains the implementation of the POSIX semaphore
//                      functions.
//              
//              
//
//####DESCRIPTIONEND####
//
//==========================================================================

#include <pkgconf/hal.h>
#include <pkgconf/kernel.h>
#include <pkgconf/posix.h>

#include <cyg/kernel/ktypes.h>          // base kernel types
#include <cyg/infra/cyg_trac.h>         // tracing macros
#include <cyg/infra/cyg_ass.h>          // assertion macros

#include <semaphore.h>                  // our header

#include "pprivate.h"                   // POSIX private header

#include <cyg/kernel/thread.hxx>        // Kernel threads

#include <cyg/kernel/thread.inl>        // Cyg_ThreadQueue::empty()

#include <cyg/kernel/sema.hxx>          // Kernel semaphores

#if CYGINT_ISO_MALLOC && defined(CYGINT_ISO_STRING_STRFUNCS)
#include <stdlib.h>
#include <fcntl.h>
#endif

// -------------------------------------------------------------------------
// Internal definitions

#if CYGINT_ISO_MALLOC && defined(CYGINT_ISO_STRING_STRFUNCS)
// Named semaphores definitions
struct sem_name
{
	char *pname;
	sem_t sem;
	int open_count;
	sem_name *pnext;
};
static sem_name *psem_list = NULL;
static Cyg_Mutex name_mutex;
#endif

// Handle entry to a pthread package function. 
#define SEMA_ENTRY() CYG_REPORT_FUNCTYPE( "returning %d" );

// Do a semaphore package defined return. This requires the error code
// to be placed in errno, and if it is non-zero, -1 returned as the
// result of the function. This also gives us a place to put any
// generic tidyup handling needed for things like signal delivery and
// cancellation.
#define SEMA_RETURN(err)                        \
CYG_MACRO_START                                 \
    int __retval = 0;                           \
    if( err != 0 ) __retval = -1, errno = err;  \
    CYG_REPORT_RETVAL( __retval );              \
    return __retval;                            \
CYG_MACRO_END

//-----------------------------------------------------------------------------
// new operator to allow us to invoke the Cyg_Thread constructor on the
// user's semaphore object.

inline void *operator new(size_t size,  void *ptr) { return (void *)ptr; };

// -------------------------------------------------------------------------
// Initialize semaphore to value.
// pshared is not supported under eCos.

externC int sem_init  (sem_t *sem, int pshared, unsigned int value)
{
    SEMA_ENTRY();

    if( value > SEM_VALUE_MAX )
        SEMA_RETURN(EINVAL);

    Cyg_Counting_Semaphore *sema;

    sema = new((void *)sem) Cyg_Counting_Semaphore(value);

    sema=sema;
    
    SEMA_RETURN(0);
}

// -------------------------------------------------------------------------
// Destroy the semaphore.

externC int sem_destroy  (sem_t *sem)
{
    SEMA_ENTRY();

    Cyg_Counting_Semaphore *sema = (Cyg_Counting_Semaphore *)sem;

    // Check that the semaphore has no waiters
    if( sema->waiting() )
        SEMA_RETURN(EBUSY);

    // Call the destructor
    sema->~Cyg_Counting_Semaphore();
    
    SEMA_RETURN(0);
}

// -------------------------------------------------------------------------
// Decrement value if >0 or wait for a post.

externC int sem_wait  (sem_t *sem)
{
    int retval = 0;
    
    SEMA_ENTRY();

#ifdef CYGPKG_POSIX_PTHREAD
    // check for cancellation first.
    pthread_testcancel();
#endif

    Cyg_Counting_Semaphore *sema = (Cyg_Counting_Semaphore *)sem;

    if( !sema->wait() ) retval = EINTR;
    
#ifdef CYGPKG_POSIX_PTHREAD
    // check if we were woken because we were being cancelled
    pthread_testcancel();
#endif

    SEMA_RETURN(retval);
}

// -------------------------------------------------------------------------
// Decrement value if >0, return -1 if not.

externC int sem_trywait  (sem_t *sem)
{
    int retval = 0;
    
    SEMA_ENTRY();

    Cyg_Counting_Semaphore *sema = (Cyg_Counting_Semaphore *)sem;

    if( !sema->trywait() ) retval = EAGAIN;
    
    SEMA_RETURN(retval);
}

// -------------------------------------------------------------------------
// Increment value and wake a waiter if one is present.

externC int sem_post  (sem_t *sem)
{
    SEMA_ENTRY();

    Cyg_Counting_Semaphore *sema = (Cyg_Counting_Semaphore *)sem;

    sema->post();
    
    SEMA_RETURN(0);
}
    

// -------------------------------------------------------------------------
// Get current value

externC int sem_getvalue  (sem_t *sem, int *sval)
{
    SEMA_ENTRY();

    Cyg_Counting_Semaphore *sema = (Cyg_Counting_Semaphore *)sem;

    *sval = sema->peek();

    CYG_REPORT_RETVAL( 0 );
    return 0;
}

#if CYGINT_ISO_MALLOC && defined(CYGINT_ISO_STRING_STRFUNCS)
// -------------------------------------------------------------------------
// Open an existing named semaphore, or create it.

externC sem_t *sem_open  (const char *name, int oflag, ...)
{
    SEMA_ENTRY();
	
	va_list arg;
	mode_t mode;
	unsigned int value = 0;
	sem_t *retval = SEM_FAILED;
	
	sem_name *psem, *plast;
	
	//Check name.
	if (strlen(name) == 0)
	{
		errno = EINVAL;
		CYG_REPORT_RETVAL(SEM_FAILED);
		return SEM_FAILED;
	}
	
	if (oflag & O_CREAT)
	{
		va_start(arg, oflag);
		mode = (mode_t)va_arg(arg, mode_t);
		value = va_arg(arg, unsigned int);
		va_end(arg);
	}
	
	name_mutex.lock();
	
	//Check for existence of named semaphore.
	for (psem = plast = psem_list; psem != NULL; psem = psem->pnext)
	{
		if (strcmp(name, psem->pname) == 0)
		break;
		plast = psem;
	}
	
	do
	{
		//The named semaphore exist.
		if (psem != NULL)
		{
			//Check for existence with creation.
			if ((oflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
			{
				errno = EEXIST;
				break;
			}
			
			//Update use count and return address of found semaphore.
			psem->open_count++;
			retval = &psem->sem;
			break;
		}
	
		//The named semaphore does not exist.
		else
		{
			//Check  for creation.
			if (!(oflag & O_CREAT))
			{
				errno = ENOENT;
				break;
			}
		
			//Allocate space for name and semaphore.
			char *pnew_sem_name = (char *)malloc(strlen(name) + 1);
			if (pnew_sem_name == NULL)
			{
				errno = ENOMEM;
				break;
			}
			sem_name *pnew_sem = (sem_name *)malloc(sizeof(sem_name));
			if (pnew_sem == NULL)
			{
				free(pnew_sem_name);
				errno = ENOMEM;
				break;
			}
		
			//Initialize semaphore.
			if (sem_init(&(pnew_sem->sem), 0, value) == -1)
			{
				free(pnew_sem_name);
				free(pnew_sem);
				break;
			}
		
			//Insert new named semaphore in list.
			strcpy(pnew_sem_name, name);
			pnew_sem->pname = pnew_sem_name;
			pnew_sem->open_count = 1;
			pnew_sem->pnext = NULL;
			if (plast == NULL)
				psem_list = pnew_sem;
			else
				plast->pnext = pnew_sem;
				
			//Return address of the new semaphore.
			retval = &pnew_sem->sem;
		}
	} while (false);
	
	name_mutex.unlock();

	CYG_REPORT_RETVAL(retval);
    return retval;
}

// -------------------------------------------------------------------------
// Close descriptor for semaphore.

externC int sem_close  (sem_t *sem)
{
    SEMA_ENTRY();
		
	sem_name *psem, *plast;
	int retval = -1;
	
	name_mutex.lock();
	
	//Check for existence of named semaphore
	//(the address is assumed to be sufficient for unique identification).
	for (psem = plast = psem_list; psem != NULL; psem = psem->pnext)
	{
		if (sem == &(psem->sem))
		break;
		plast = psem;
	}
	
	do
	{
		//Named sempahore does not exist.
		if (psem == NULL)
		{
			errno = EINVAL;
			break;
		}
		
		//Remove existing semaphore (if this is the last close and already unlinked).
		if ((psem->open_count == 1) && (strlen(psem->pname) == 0))
		{
			if (sem_destroy(&(psem->sem)) == -1)
			{
				retval = -1;
				break;
			}
			if (psem == plast)
				psem_list = psem->pnext;
			else
				plast->pnext = psem->pnext;
			
			free(psem->pname);
			free(psem);
		}
		else
		
		//Do not remove (there are more users and/or not yet unlinked).
		if (psem->open_count > 0)
			psem->open_count--;;
			
		retval = 0;
		
	} while (false);
	
	name_mutex.unlock();

	CYG_REPORT_RETVAL(retval);
	return retval;
}   

// -------------------------------------------------------------------------
// Remove named semaphore

externC int sem_unlink  (const char *name)
{
    SEMA_ENTRY();
		
	sem_name *psem, *plast;
	int retval = -1;
	
	//Check name.
	if (strlen(name) == 0)
	{
		errno = ENOENT;
		CYG_REPORT_RETVAL(retval);
		return retval;
	}
	
	name_mutex.lock();
	
	//Check for existence of named semaphore.
	for (psem = plast = psem_list; psem != NULL; psem = psem->pnext)
	{
		if (strcmp(name, psem->pname) == 0)
		break;
		plast = psem;
	}
	
	do
	{
		if (psem == NULL)
		{
			errno = ENOENT;
			break;
		}
		
		//Remove semaphore?
		if (psem->open_count == 0)
		{
			if (sem_destroy(&(psem->sem)) == -1)
			{
				retval = -1;
				break;
			}
			if (psem == plast)
				psem_list = psem->pnext;
			else
				plast->pnext = psem->pnext;
			
			free(psem->pname);
			free(psem);
			
		}
		else
			psem->pname[0] = 0;
			
		retval = 0;
		
	} while (false);

	name_mutex.unlock();
	
	CYG_REPORT_RETVAL(retval);
	return retval;
} 
#else

// -------------------------------------------------------------------------
// Open an existing named semaphore, or create it.

externC sem_t *sem_open  (const char *name, int oflag, ...)
{
    SEMA_ENTRY();

    errno = ENOSYS;

    CYG_REPORT_RETVAL( SEM_FAILED );
    return SEM_FAILED;
}

// -------------------------------------------------------------------------
// Close descriptor for semaphore.

externC int sem_close  (sem_t *sem)
{
    SEMA_ENTRY();

    SEMA_RETURN(ENOSYS);
}   

// -------------------------------------------------------------------------
// Remove named semaphore

externC int sem_unlink  (const char *name)
{
    SEMA_ENTRY();
	

    SEMA_RETURN(ENOSYS);
} 
#endif

// -------------------------------------------------------------------------
// EOF sem.cxx
