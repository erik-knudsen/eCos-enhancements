/* Erik Aagaard Knudsen.

  Revision History:
  22-feb-2010 eak: Original

  Abstract: Configuration for Spider Monkey.

  Platforms: eCos.

  $Id: jsautocfg.h,v 1.1 2010/02/26 14:23:00 erik Exp $
  $Name:  $
  */

#ifndef js_autocfg___
#define js_autocfg___

#include <pkgconf/spidermonkey.h>
#include <cyg/infra/cyg_type.h>

#if (CYG_BYTEORDER == CYG_LSBFIRST)
#define IS_LITTLE_ENDIAN 1
#undef IS_BIG_ENDIAN
#else
#undef IS_LITTLE_ENDIAN
#define IS_BIG_ENDIAN 1
#endif

#define JS_BYTES_PER_BYTE   1L
#define JS_BYTES_PER_SHORT  2L

#define JS_BITS_PER_BYTE    8L
#define JS_BITS_PER_SHORT   16L

#define JS_BITS_PER_BYTE_LOG2   3L
#define JS_BITS_PER_SHORT_LOG2  4L

#define JS_ALIGN_OF_POINTER CYGARC_ALIGNMENT

#if (JS_BYTES_PER_INT == 2)
#define JS_BITS_PER_INT     16
#define JS_BITS_PER_INT_LOG2    5
#elif (JS_BYTES_PER_INT == 4)
#define JS_BITS_PER_INT     32
#define JS_BITS_PER_INT_LOG2    6
#else
# error No suitable integer type
#endif

#if (JS_BYTES_PER_LONG == 4)
#define JS_BITS_PER_LONG     32
#define JS_BITS_PER_LONG_LOG2    5
#elif (JS_BYTES_PER_LONG == 8)
#define JS_BITS_PER_LONG     32
#define JS_BITS_PER_LONG_LOG2    64
#else
# error No suitable long type
#endif

#if (JS_BYTES_PER_WORD == 4)
#define JS_BITS_PER_WORD     32
#define JS_BITS_PER_WORD_LOG2    5
#elif (JS_BYTES_PER_WORD == 8)
#define JS_BITS_PER_WORD     32
#define JS_BITS_PER_WORD_LOG2    64
#else
# error No suitable word type
#endif

#endif /* js_autocfg___ */
