#include "malloc.h"
#include <sys/mman.h>
#include <unistd.h>

static int page_size = -1;

/** This function is supposed to lock the memory data structures. It
* could be as simple as disabling interrupts or acquiring a spinlock.
* It's up to you to decide.
*
* \return 0 if the lock was acquired successfully. Anything else is
* failure.
*/
int libhalloc_lock()
{
    return 0;
}

/** This function unlocks what was previously locked by the liballoc_lock
* function. If it disabled interrupts, it enables interrupts. If it
* had acquiried a spinlock, it releases the spinlock. etc.
*
* \return 0 if the lock was successfully released.
*/
int libhalloc_unlock()
{
    return 0;
}

/** This is the hook into the local system which allocates pages. It
* accepts an integer parameter which is the number of pages
* required. The page size was set up in the liballoc_init function.
*
* \return NULL if the pages were not allocated.
* \return A pointer to the allocated memory.
*/
void* libhalloc_alloc(size_t pages)
{
    if ( page_size < 0 ) page_size = getpagesize();
    uint32_t size = pages * page_size;

    char *p2 = (char*)mmap(0, size, PROT_NONE, MAP_PRIVATE|MAP_NORESERVE|MAP_ANONYMOUS, -1, 0);
    if ( p2 == MAP_FAILED) return NULL;

    if(mprotect(p2, size, PROT_READ|PROT_WRITE) != 0)
    {
        munmap(p2, size);
        return NULL;
    }

    return p2;
}

/** This frees previously allocated memory. The void* parameter passed
* to the function is the exact same value returned from a previous
* liballoc_alloc call.
*
* The integer value is the number of pages to free.
*
* \return 0 if the memory was successfully freed.
*/
int libhalloc_free(void* ptr, size_t pages)
{
    return munmap( ptr, pages * page_size );
}
