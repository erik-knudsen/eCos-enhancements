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

/**
** EAK 17-feb-2010: Modified from NSPR with just sufficient
**                  functionality to support Spider Monkey.
**/


#include "prlock.h"
#include "pratom.h"

static PRLock *atomic_lock = NULL;
static PRBool _pr_initialized = PR_FALSE;

static void _PR_MD_INIT_ATOMIC(void)
{
    if (atomic_lock == NULL) {
        atomic_lock = PR_NewLock();
    }
    _pr_initialized = PR_TRUE;
}

static PRInt32 _PR_MD_ATOMIC_INCREMENT(PRInt32 *val)
{
    PRInt32 rv;

    if (!_pr_initialized) {
    	_PR_MD_INIT_ATOMIC();
    }
    PR_Lock(atomic_lock);
    rv = ++(*val);
    PR_Unlock(atomic_lock);
    return rv;
}

static PRInt32 _PR_MD_ATOMIC_ADD(PRInt32 *ptr, PRInt32 val)
{
    PRInt32 rv;

    if (!_pr_initialized) {
    	_PR_MD_INIT_ATOMIC();
    }
    PR_Lock(atomic_lock);
    rv = ((*ptr) += val);
    PR_Unlock(atomic_lock);
    return rv;
}

static PRInt32 _PR_MD_ATOMIC_DECREMENT(PRInt32 *val)
{
    PRInt32 rv;

    if (!_pr_initialized) {
    	_PR_MD_INIT_ATOMIC();
    }
    PR_Lock(atomic_lock);
    rv = --(*val);
    PR_Unlock(atomic_lock);
    return rv;
}

static PRInt32 _PR_MD_ATOMIC_SET(PRInt32 *val, PRInt32 newval)
{
    PRInt32 rv;

    if (!_pr_initialized) {
    	_PR_MD_INIT_ATOMIC();
    }
    PR_Lock(atomic_lock);
    rv = *val;
    *val = newval;
    PR_Unlock(atomic_lock);
    return rv;
}

PRInt32 PR_AtomicIncrement(PRInt32 *val)
{
    return _PR_MD_ATOMIC_INCREMENT(val);
}

PRInt32 PR_AtomicDecrement(PRInt32 *val)
{
    return _PR_MD_ATOMIC_DECREMENT(val);
}

PRInt32 PR_AtomicSet(PRInt32 *val, PRInt32 newval)
{
    return _PR_MD_ATOMIC_SET(val, newval);
}

PRInt32 PR_AtomicAdd(PRInt32 *ptr, PRInt32 val)
{
    return _PR_MD_ATOMIC_ADD(ptr, val);
}
