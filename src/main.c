#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "malloc.h"

extern int malloc_random_test( int verbose );

int test_malloc_free_int()
{
    int* var;

    printf("test_malloc_free_int\n");

    var = malloc(sizeof(int));

    assert(var != NULL);    // Success
    assert( ((uintptr_t)var & 15) == 0); // 16 byte aligned
    *var = 42;
    assert(*var == 42);      // Wrote value

    free(var);

    return 0;
}

int test_malloc_free_5int()
{
    int* var[5];
    int i;

    printf("test_malloc_free_5int\n");

    for (i=0; i<5; i++)
    {
        var[i] = malloc(sizeof(int));
        assert(var[i] != NULL);                    // Success
        assert( ((uintptr_t)var[i] & 15) == 0 ); // 16 byte aligned
    }

    for (i=0; i<5; i++)
    {
        *var[i] = 42 + i;
        assert((*var[i] == (42 + i)));             // Wrote value
    }

    for (i=0; i<5; i++)
    {
        free(var[i]);
    }

    return 0;
}

int test_malloc_free_coallesce_left(int size)
{
    uintptr_t addr;
    int*      var[3];

    printf("test_malloc_free_coallesce_left(%d)\n", size);

    var[0] = malloc(size);
    var[1] = malloc(size);
    var[2] = malloc(size);

    assert(var[0] != NULL);
    assert(var[1] != NULL);
    assert(var[2] != NULL);

    assert(((uintptr_t)var[0] & 15) == 0);
    assert(((uintptr_t)var[1] & 15) == 0);
    assert(((uintptr_t)var[2] & 15) == 0);

    memset(var[0],0,size);
    memset(var[1],0,size);
    memset(var[2],0,size);

    addr = (uintptr_t) var[0];

    free (var[1]);
    free (var[0]);  // coallesce left

    var[0] = malloc(size*2);
    memset(var[0],0,size*2);

    assert(addr == (uintptr_t)var[0]);

    free(var[0]);
    free(var[2]);

    return 0;
}

int test_malloc_free_coallesce_right(int size)
{
    uintptr_t addr;
    int*      var[4];

    printf("test_malloc_free_coallesce_right(%d)\n", size);

    var[0] = malloc(size);
    var[1] = malloc(size);
    var[2] = malloc(size);
    var[3] = malloc(size);

    assert(var[0] != NULL);
    assert(var[1] != NULL);
    assert(var[2] != NULL);
    assert(var[3] != NULL);

    assert(((uintptr_t)var[0] & 15) == 0);
    assert(((uintptr_t)var[1] & 15) == 0);
    assert(((uintptr_t)var[2] & 15) == 0);
    assert(((uintptr_t)var[3] & 15) == 0);

    memset(var[0],0,size);
    memset(var[1],0,size);
    memset(var[2],0,size);
    memset(var[3],0,size);

    addr = (uintptr_t) var[2];

    free (var[2]);
    free (var[3]);  // coallesce right

    var[2] = malloc(size*2);
    memset(var[2],0,size*2);

    assert(addr == (uintptr_t)var[2]);

    free(var[0]);
    free(var[1]);
    free(var[2]);

    return 0;
}

int test_malloc_free_coallesce_leftright(int size)
{
    uintptr_t addr;
    int*      var[4];

    printf("test_malloc_free_coallesce_leftright(%d)\n", size);

    var[0] = malloc(size);
    var[1] = malloc(size);
    var[2] = malloc(size);
    var[3] = malloc(size);

    assert(var[0] != NULL);
    assert(var[1] != NULL);
    assert(var[2] != NULL);
    assert(var[3] != NULL);

    assert(((uintptr_t)var[0] & 15) == 0);
    assert(((uintptr_t)var[1] & 15) == 0);
    assert(((uintptr_t)var[2] & 15) == 0);
    assert(((uintptr_t)var[3] & 15) == 0);

    memset(var[0],0,size);
    memset(var[1],0,size);
    memset(var[2],0,size);
    memset(var[3],0,size);

    addr = (uintptr_t) var[1];

    free(var[2]);
    free(var[1]);   // coallesce left
    free(var[3]);   // coallesce right

    var[1] = malloc(size*3);
    memset(var[1],0,size*3);

    assert(addr == (uintptr_t)var[1]);

    free(var[0]);
    free(var[1]);

    return 0;
}

int main( int argc, char *argv )
{
    int verbose = argc > 1;

    printf("%s\n","memory testing application" );

    test_malloc_free_int();
    test_malloc_free_5int();

    test_malloc_free_coallesce_left(64);
    test_malloc_free_coallesce_right(64);
    test_malloc_free_coallesce_leftright(64);

    test_malloc_free_coallesce_left(4096);
    test_malloc_free_coallesce_right(4096);
    test_malloc_free_coallesce_leftright(4096);

    malloc_random_test( verbose );

    mallocstats();

    printf("%s\n","all tests passed!");

    return 0;
}
