/*
 * The executable is linked without the C runtime library. The compiler still emits calls to
 * memset and memcpy for structure initialization and copies, so they are provided here.
 * The string instructions keep the compiler from turning these bodies back into calls.
 */
#include <intrin.h>
#include <string.h>

#pragma function(memset)
void *memset(void *destination, int value, size_t count)
{
    __stosb((unsigned char *)destination, (unsigned char)value, count);
    return destination;
}

#pragma function(memcpy)
void *memcpy(void *destination, const void *source, size_t count)
{
    __movsb((unsigned char *)destination, (const unsigned char *)source, count);
    return destination;
}
