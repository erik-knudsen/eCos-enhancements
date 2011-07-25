/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Netscape Portable Runtime (NSPR).
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998-2000
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
** File:            ptsynch.c
** Description:        Implementation for thread synchronization using pthreads.
**
** EAK 17-feb-2010: Modified from NSPR with just sufficient
**                  functionality to support Spider Monkey.
*/

#include "prcvar.h"

#include <pthread.h>
#include <assert.h>
#include <stdlib.h>

PRLock* PR_NewLock(void)
{
    int rv;
    PRLock *lock = malloc(sizeof(PRLock));
    if (lock != NULL)
    {
        rv = pthread_mutex_init(&lock->mutex, NULL);
        assert(0 == rv);
    }
    return lock;
}

void PR_DestroyLock(PRLock *lock)
{
    assert(NULL != lock);
    int rv = pthread_mutex_destroy(&lock->mutex);
    assert(0 == rv);
    free(lock);
}

void PR_Lock(PRLock *lock)
{
    assert(lock != NULL);
    int rv = pthread_mutex_lock(&lock->mutex);
    assert(0 == rv);
}

PRStatus PR_Unlock(PRLock *lock)
{
    assert(lock != NULL);
    int rv = pthread_mutex_unlock(&lock->mutex);
    assert(0 == rv);

    return PR_SUCCESS;
}

PRCondVar* PR_NewCondVar(PRLock *lock)
{
    assert(lock != NULL);
    PRCondVar *cvar = malloc(sizeof(PRCondVar));
    if (cvar != NULL)
    {
        int rv = pthread_cond_init(&cvar->cv, NULL);
        assert(0 == rv);
        cvar->lock = lock;
    }
    return cvar;
}

void PR_DestroyCondVar(PRCondVar *cvar)
{
    int rv = pthread_cond_destroy(&cvar->cv);
    assert(0 == rv);
    free(cvar);
}

//Differs from the original in that it is required that timeout has the value
//PR_INTERVAL_NO_TIMEOUT. Spider Monkey only uses this value.
PRStatus PR_WaitCondVar(PRCondVar *cvar, PRIntervalTime timeout)
{
    int rv;

    assert(cvar != NULL);
    assert(timeout == PR_INTERVAL_NO_TIMEOUT);
    assert((EBUSY == pthread_mutex_trylock(&(cvar->lock->mutex))));

    rv = pthread_cond_wait(&cvar->cv, &cvar->lock->mutex);
    if (rv != 0)
        return PR_FAILURE;

    return PR_SUCCESS;
}

PRStatus PR_NotifyCondVar(PRCondVar *cvar)
{
    assert(cvar != NULL);
    int rv = pthread_cond_signal(&cvar->cv);
    assert(0 == rv);
    
    return PR_SUCCESS;
}

PRStatus PR_NotifyAllCondVar(PRCondVar *cvar)
{
    assert(cvar != NULL);
    int rv = pthread_cond_broadcast(&cvar->cv);
    assert(0 == rv);
    
    return PR_SUCCESS;
}
