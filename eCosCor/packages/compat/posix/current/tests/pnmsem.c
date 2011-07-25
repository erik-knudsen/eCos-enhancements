//==========================================================================
//
//        pnmsem.c
//
//        Named semaphore test
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
// Author(s):     eak
// Contributors:
// Date:          2009-03-1
// Description:   Test named semaphores
//
//####DESCRIPTIONEND####

// ------------------------------------------------------------------------

#include <cyg/infra/testcase.h>
#include <pkgconf/posix.h>
#include <pkgconf/system.h>
#include <errno.h>
#ifdef CYGPKG_KERNEL
#include <pkgconf/kernel.h>
#endif

#ifdef CYGPKG_ISOINFRA
# include <sys/types.h>
# include <pthread.h>
# include <semaphore.h>
# include <fcntl.h>
# include <time.h>
# include <unistd.h>
#endif

#if !defined(CYGPKG_POSIX_PTHREAD)
#define NA_MSG "POSIX threads not enabled"
#elif !defined(_POSIX_SEMAPHORES)
#define NA_MSG "No POSIX sempaphore support enabled"
#elif !defined(CYGFUN_KERNEL_API_C)
#define NA_MSG "Kernel C API not enabled"
#endif

#ifdef NA_MSG
void
cyg_start(void)
{
    CYG_TEST_INIT();
    CYG_TEST_NA(NA_MSG);
}
#else

#include <cyg/infra/cyg_ass.h>
#include <cyg/infra/cyg_trac.h>
#include <cyg/infra/diag.h>             // diag_printf

#include <cyg/kernel/kapi.h>            // Some extras

// ------------------------------------------------------------------------
// Management functions

#define STACKSIZE (PTHREAD_STACK_MIN*6)

static pthread_t thread;

typedef CYG_WORD64 CYG_ALIGNMENT_TYPE;

static CYG_ALIGNMENT_TYPE stack[
   (STACKSIZE+sizeof(CYG_ALIGNMENT_TYPE)-1)
     / sizeof(CYG_ALIGNMENT_TYPE)];

static char name1[] = "sem_name_1";
static char name2[] = "sem_name_2";
static sem_t main_sem;


static pthread_t new_thread( void *(*entry)(void *))
{
    pthread_attr_t attr;

    pthread_attr_init( &attr );
    pthread_attr_setstackaddr( &attr, (void *)((char *)(&stack)+STACKSIZE) );
    pthread_attr_setstacksize( &attr, STACKSIZE );
    pthread_attr_setinheritsched( &attr, PTHREAD_EXPLICIT_SCHED );
    pthread_attr_setschedpolicy( &attr, SCHED_RR );

    pthread_create( &thread,
                    &attr,
                    entry,
                    0);

    return thread;
}


static void *test_thread( void *arg )
{
	sem_t *sem1, *sem2;
	int ret;

	sem2 = sem_open(name2, O_CREAT, 0, 1);

    CYG_TEST_CHECK(sem2 !=  SEM_FAILED, "sem_open failed in test thread");

    ret = sem_wait(sem2);

    CYG_TEST_CHECK(ret ==  0, "sem_wait failed in test thread");

    sem1 = sem_open(name1, 0);

    CYG_TEST_CHECK(sem1 !=  SEM_FAILED, "sem_open failed in test thread");

    ret = sem_post(sem1);

    CYG_TEST_CHECK(ret ==  0, "sem_post failed in test thread");

    ret = sem_wait(sem2);

    CYG_TEST_CHECK(ret == 0, "sem_wait failed in test thread");

    ret= sem_unlink(name2);

    CYG_TEST_CHECK(ret == 0, "sem_unlink failed in test thread");

    ret = sem_close(sem2);

    CYG_TEST_CHECK(ret == 0, "sem_close failed in test thread");

    ret = sem_post(sem1);

    CYG_TEST_CHECK(ret == 0, "sem_post failed in test thread");

    ret =sem_close(sem1);

    CYG_TEST_CHECK(ret ==  0, "sem_close failed in test thread");

    sem_post(&main_sem);

    return NULL;
}

// ------------------------------------------------------------------------

externC int
main( int argc, char **argv )
{
	sem_t *sem1, *sem2;
	int ret;

    CYG_TEST_INIT();

    CYG_TEST_INFO ("Testing named semaphore functions");

	sem1 = sem_open("", O_CREAT, 0, 0);

    CYG_TEST_CHECK((sem1 ==  SEM_FAILED) && (errno == EINVAL), "sem_open did not fail with EINVAL");

	sem1 = sem_open(name1, 0);

    CYG_TEST_CHECK((sem1 ==  SEM_FAILED) && (errno == ENOENT), "sem_open did not fail with ENOENT");

	sem1 = sem_open(name1, O_CREAT, 0, 0);

    CYG_TEST_CHECK(sem1 !=  SEM_FAILED, "sem_open failed");

	sem2 = sem_open(name1, O_CREAT | O_EXCL, 0, 0);

    CYG_TEST_CHECK((sem2 ==  SEM_FAILED) && (errno == EEXIST), "sem_open did not fail with EEXIST");

	sem2 = sem_open(name1, 0);

    CYG_TEST_CHECK(sem1 ==  sem2, "sem_open did not return same semaphore");

    ret = sem_close(sem2);

    CYG_TEST_CHECK(ret == 0, "sem_close failed");

    ret = sem_close(sem1);

    CYG_TEST_CHECK(ret == 0, "sem_close failed");

    ret = sem_close(sem1);

    CYG_TEST_CHECK(ret == 0, "sem_close failed");

    sem2 = sem_open(name1, 0);

    CYG_TEST_CHECK(sem1 ==  sem2, "sem_open did not open same semaphore");

    ret = sem_unlink("");

    CYG_TEST_CHECK((ret == -1) && (errno == ENOENT), "sem_unlink did not fail with ENOENT");

    ret = sem_unlink(name2);

    CYG_TEST_CHECK((ret == -1) && (errno == ENOENT), "sem_unlink did not fail with ENOENT");

    ret = sem_close(sem2);

    CYG_TEST_CHECK(ret == 0, "sem_close failed");

    sem1 = sem_open(name1, O_CREAT, 0, 0);

    CYG_TEST_CHECK(sem1 != SEM_FAILED, "sem_open failed");

    new_thread(test_thread);

    ret = sem_wait(sem1);

    CYG_TEST_CHECK(ret == 0, "sem_wait error");

    sem2 = sem_open(name2, 0);

    CYG_TEST_CHECK(sem2 !=  SEM_FAILED, "sem_open failed");

    ret = sem_post(sem2);

    CYG_TEST_CHECK(ret == 0, "sem_post error");

    ret = sem_close(sem2);

    CYG_TEST_CHECK(ret == 0, "sem_close error");

    ret = sem_wait(sem1);

    CYG_TEST_CHECK(ret == 0, "sem_wait error");

    ret = sem_close(sem1);

    CYG_TEST_CHECK(ret == 0, "sem_close error");

    ret = sem_unlink(name1);

    CYG_TEST_CHECK(ret == 0, "sem_unlink error");

    sem_init(&main_sem, 0, 0);
    sem_wait(&main_sem);

    CYG_TEST_PASS_FINISH( "Named semaphores" );


    return 0;
}

#endif

// EOF pnmsem.c
