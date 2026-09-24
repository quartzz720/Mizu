#ifndef MP3SHIM_H
#define MP3SHIM_H

/* What minimp3.h expects from a C library, in a system that has none.
 *
 * The decoder is written against <stdint.h>, <stdlib.h> and <string.h> and
 * uses almost nothing from them: the fixed-width types, and memcpy/memset.
 * This is that, and no more - which is also a fair description of how much of
 * a C library anything here has ever needed.
 *
 * Kept beside the decoder rather than inside it, so that minimp3.h stays as
 * close as possible to what it was upstream. The only changes made to that
 * file are the two includes at the top; everything else it needs is here.
 */

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

void* memcpy(void* to, const void* from, unsigned long long length);
void* memset(void* at, int value, unsigned long long length);

#endif
