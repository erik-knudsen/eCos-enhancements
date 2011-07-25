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
** File:                prtypes.h
** Description: Definitions of NSPR's basic types
**
** Prototypes and macros used to make up for deficiencies that we have found
** in ANSI environments.
**
** Since we do not wrap <stdlib.h> and all the other standard headers, authors
** of portable code will not know in general that they need these definitions.
** Instead of requiring these authors to find the dependent uses in their code
** and take the following steps only in those C files, we take steps once here
** for all C files.
**
** EAK 17-feb-2010: Modified from NSPR with just sufficient functionality to
**                  support Spider Monkey on CPU's with the following types.
**                  Originally this is derived from prcpucfg.h.
**/

#ifndef prtypes_h___
#define prtypes_h___

#include <stddef.h>

#include "jstypes.h"


#ifdef __cplusplus
extern "C"
{
#endif

//8 bit integer signed and unsigned (not used).
#define PRUint8 JSUint8
#define PRInt8 JSInt8

//16 bit integer signed and unsigned (not used).
#define PRUint16 JSUint16
#define PRInt16 JSInt16

//32 bit integer signed and unsigned.
#define PRUint32 JSUint32
#define PRInt32 JSInt32

//Automatic integer signed and unsigned.
#define PRIntn JSIntn;
#define PRUintn JSUintn

//Bool.
#define PRBool JSBool
#define PR_TRUE JS_TRUE
#define PR_FALSE JS_FALSE

typedef enum { PR_FAILURE = -1, PR_SUCCESS = 0 } PRStatus;

#define PR_INTERVAL_NO_TIMEOUT 0xffffffffUL
typedef PRUint32 PRIntervalTime;


#ifdef __cplusplus
}
#endif

#endif /* prtypes_h___ */

