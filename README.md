halloc - Helix Memory Allocator
===============================

This is a custom heap memory allocator and manager for use in hosted or nonhosted applications. The intended use of this allocator is on kernel development.

Project
-------

The allocator uses a segregated free-list approach to track the non-allocated regions of the heap. It also is designed to be cappable of handling a non-contiguous heap, i.e., a heap with non-contiguous pages spread accross the memory.

The free-lists are divided in 6 classes, each one handling a free region with similar sizes. The classes are:
* Free regions with size <= 32 bytes;
* Free regions with size <= 32 bytes;
* Free regions with size <= 32 bytes;

The block of contiguous pages (heap block) are tracked in a double-linked list structure. Inside each heap block there are regions, or allocated or free. Each allocated region have a header and a footer containing two field of information:

* The size in bytes of the region (header size + footer size + payload size);
* The status of the region: a free region contains the value 1, an allocated region contains the value 0;

In case the region is a free region, the header contains two more fields: a next and previous pointers to point other free regions that have the same class sizes.
