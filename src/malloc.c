#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "malloc.h"

/*************************************************************************************************/
/*********************************** Constants definitions ***************************************/

#define MINIMUM_REGION_SIZE         16                          // Each region contains at least 16 bytes
#define PAGE_SIZE                   4096                        // x86 page size in bytes
#define FREE_BLOCKS_SETS            6                           /* How many sets of free blocks we want.
                                                                 * Starting in >= 16 and <=32 bytes */
#define LARGE_FREE_BLOCK_INDEX      FREE_BLOCKS_SETS-1          // Index of the last set of free blocks
#define REGION_OVERHEAD_SIZE        (sizeof(AllocMetadata_t)*2) // 8 bytes of overhead (region's size headers)
#define REGION_PAYLOAD_SIZE(x)      (x-REGION_OVERHEAD_SIZE)    // How much payload (data+padding) this region holds
#define PAYLOAD_WITH_OVERHEAD(x)    (x+REGION_OVERHEAD_SIZE)    // Total size of this allocation

/*************************************************************************************************/
/*********************************** Structs definitions *****************************************/

/**
 * All the allocated/free regions are 16 byte-aligned and contains a header and
 * a footer of allocmetadata_t type.
 *************************************************************************************************/
typedef struct AllocMetadata_s
{
    uint32_t used: 4;   // Used to know if this region is alocated(1) or free(0)
    uint32_t size: 28;  // Size of this region

} AllocMetadata_t; // 4 bytes

/**
 * Header of the free regions. All the free regions are 16 byte-aligned and
 * contains a header of freemetadata_t type and a footer of allocmetadata_t type.
 *************************************************************************************************/
typedef struct FreeRegionHeader_s
{
    AllocMetadata_t metadata;
#if defined(__x86_64__) || defined(_M_X64_) // Keeps metadata aligned to first 4 bytes when in 64 bits
    uint32_t    __reserved;
#endif
    struct FreeRegionHeader_s* next;        // Next free region in this block
    struct FreeRegionHeader_s* previous;    // Previous free region in this block

} FreeRegionHeader_t; // 12 bytes (32 bits) / 24 aligned bytes (64 bits)

/**
 * The header used in the blocks retrieved from the OS.
 * Each block (1 or more pages) must have this header in the block begining.
 *************************************************************************************************/
typedef struct BlockHeader_s
{
    uint32_t pages;                 // Pages allocated from the system
    uint32_t size;                  // Total size allocated from the system
    uint32_t usedSize;              // Size allocated to the client
    struct BlockHeader_s* next;     // Next block given from OS
    struct BlockHeader_s* previous; // Previous block given from OS

    FreeRegionHeader_t* freeRegions[FREE_BLOCKS_SETS];  /* >=16 and <=32 (8 to 24 bytes payload),  *
                                                       * <=64          (60 bytes payload),       *
                                                       * <=128         (120 bytes payload),      *
                                                       * <=256         (248 bytes payload),      *
                                                       * <=512         (504 bytes payload),      *
                                                       * > 512         (504 bytes payload)       */
} BlockHeader_t; // 44 bytes (32 bits) / 80 aligned bytes (64 bits)

/*************************************************************************************************/
/*********************************** Global variables ********************************************/

/**
 * @brief firstBlock First block of memory catch from the kernel
 *************************************************************************************************/
static BlockHeader_t* blockList = NULL;

/**
 * @brief blockEmptySize Size of overhead (BlockHeader_t + Alignment) in a empty BlockHeader_t
 *************************************************************************************************/
static uint32_t       emptyBlockOverheadSize = 0;

/*************************************************************************************************/
/*********************************** Methods prototypes ******************************************/

static FreeRegionHeader_t* FreeRegion_create             (void* start, size_t size);
static FreeRegionHeader_t* FreeRegion_split              (FreeRegionHeader_t* original, size_t size);
static size_t              FreeRegion_getSizeForAlignment(FreeRegionHeader_t* original, size_t size);

static BlockHeader_t*      Block_create(size_t size);

static uint32_t            Block_isFreeRegion            (BlockHeader_t* _this, AllocMetadata_t* addr);
static void                Block_coallesceBothSides      (BlockHeader_t* _this, AllocMetadata_t* left, FreeRegionHeader_t* reference, AllocMetadata_t* right);
static void                Block_coallesceLeftSide       (BlockHeader_t* _this, AllocMetadata_t* left, FreeRegionHeader_t* reference);
static void                Block_coallesceRightSide      (BlockHeader_t* _this, FreeRegionHeader_t* reference, AllocMetadata_t* right);
static void                Block_coallesceFreeRegion     (BlockHeader_t* _this, FreeRegionHeader_t *freeRegion);
static uint32_t            Block_addRegionToFreeList     (BlockHeader_t* _this, FreeRegionHeader_t* item);
static uint32_t            Block_removeRegionFromFreeList(BlockHeader_t* _this, FreeRegionHeader_t* item);
static AllocMetadata_t*    Block_useRegion               (BlockHeader_t* _this, FreeRegionHeader_t* freeRegion);
static FreeRegionHeader_t* Block_freeRegion              (BlockHeader_t* _this, AllocMetadata_t* region);
static uint32_t            Block_isFull                  (BlockHeader_t* _this);
static FreeRegionHeader_t* Block_canAllocateSize         (BlockHeader_t* _this, uint32_t size);
static AllocMetadata_t*    Block_allocateRegion          (BlockHeader_t* _this, size_t size);
static void Block_deallocateRegion(BlockHeader_t* _this, AllocMetadata_t* region);

static uint32_t            BlockList_addBlockToList      (BlockHeader_t **list, BlockHeader_t* item);
static uint32_t            BlockList_removeBlockFromList (BlockHeader_t** list, BlockHeader_t* item);

/*************************************************************************************************/
/*********************************** Utilitary functions *****************************************/

/**
 * @brief toFreeListIndex Return the free list vector index relatively to size informed
 * @param size            Size of the region
 * @return                Free list vector index
 *************************************************************************************************/
static uint32_t toFreeListIndex(size_t size)
{
    uint32_t s = size;

    if (s <= 32) return 0;
    if (s <= 64) return 1;
    if (s <= 128) return 2;
    if (s <= 256) return 3;
    if (s <= 512) return 4;

    return 5; // if (s > 512)
}

/**
 * @brief InitializeHeap Setup a new heap block for first use
 * @param size           Size of the block
 * @return               New heap block
 *************************************************************************************************/
static BlockHeader_t* createHeapBlock(size_t size)
{
    BlockHeader_t*   block;
    size_t alignRegionSize = (sizeof(uintptr_t)*2); /* Free block payload (next and prev pointers) */

    block = Block_create(size);

    // Create First Block for alignment
    Block_allocateRegion(block, alignRegionSize);

    return block;
}

/**
 * @brief getBlockWithFreeRegion Search for a block with a free region with can hold a payload of
 *                               informed size
 * @param size                   Size of the payload user requested
 * @return                       A block with a free region or NULL
 *************************************************************************************************/
static BlockHeader_t* getBlockWithFreeRegion(size_t size)
{
    BlockHeader_t* block = NULL;

    if (blockList == NULL)
    {
        block = createHeapBlock(PAGE_SIZE*4);
        BlockList_addBlockToList(&blockList, block);
        emptyBlockOverheadSize = block->usedSize;
        return block;
    }

    // Search for a block with free regions to use
    for(block = blockList;
        (block != NULL);
        block = block->next)
    {
        if (!Block_isFull(block) && Block_canAllocateSize(block, size))
        {
            return block;
        }
    }

    if (block == NULL)
    {
        // Allocate a new block
        block = createHeapBlock(size);
        BlockList_addBlockToList(&blockList, block);
        return block;
    }

    return NULL;
}

/**
 * @brief getBlockWithRegion Using the address informed, search for a heap block which contains it.
 * @param region             Region address to be searched
 * @return                   The heap block which contains it or null
 *************************************************************************************************/
static BlockHeader_t* getBlockWithRegion(void* region)
{
    BlockHeader_t* block;

    // Search for a block with free regions to use
    for(block = blockList;
        block != NULL;
        block = block->next)
    {
        uintptr_t regionAddr     = (uintptr_t)region;
        uintptr_t blockStartAddr = (uintptr_t)block;
        uintptr_t blockEndAddr   = blockStartAddr + block->size;

        if (regionAddr >= blockStartAddr && regionAddr < blockEndAddr)
        {
            return block;
        }
    }

    return NULL;
}

/**
 * @brief BlockList_addBlockToList Add a block heap to the block heap linked list
 * @param list                     The block heap linked list
 * @param item                     The item to be added
 * @return
 *************************************************************************************************/
static uint32_t BlockList_addBlockToList(BlockHeader_t** list, BlockHeader_t* item)
{
    BlockHeader_t* aux = *list;

    if (item == NULL)
    {
        return 0;
    }

    if (*list == NULL)
    {
        item->next     = NULL;
        item->previous = NULL;
        *list = item;
        return 0;
    }

    if (*list > item)
    {
        item->next     = *list;
        item->previous = NULL;
        (*list)->previous = item;
        *list = item;
        return 0;
    }

    // Get the last item in list
    // Order by region address
    while ((aux->next != NULL) && (item < aux->next))
    {
        aux = aux->next;
    }

    item->next     = aux->next;
    item->previous = aux;
    aux->next      = item;

    return 0;
}

/**
 * @brief BlockList_removeBlockFromList Remove a block heap to the block heap linked list
 * @param list                          The block heap linked list
 * @param item                          The item to be removed
 * @return
 *************************************************************************************************/
static uint32_t BlockList_removeBlockFromList(BlockHeader_t** list, BlockHeader_t* item)
{
    BlockHeader_t* aux;

    if (item == NULL)
    {
        return -1;
    }

    // Case 1: first element of the list
    if (*list == item)
    {
        BlockHeader_t* next = item->next;

        (*list) = next;

        if (next != NULL)
        {
            next->previous = NULL;
        }

        item->next      = NULL;
        item->previous  = NULL;

        return 0;
    }

    // Case 2: element in the middle of the list
    for (aux = *list; aux != NULL; aux = aux->next)
    {
        if (aux->next == item)
        {
            BlockHeader_t* next = item->next;

            aux->next = next;

            if (next != NULL)
            {
                next->previous = aux;
            }

            break;
        }
    }

    item->next      = NULL;
    item->previous  = NULL;

    return 0;
}

/*************************************************************************************************/
/*********************************** Free Region Methods *****************************************/

/**
 * @brief FreeRegion_create Setup a new free region delimited by the start and size params.
 * @param start             Address which the free region will begin
 * @param size              Size of the free region
 * @return                  Pointer to the header
 *************************************************************************************************/
static FreeRegionHeader_t* FreeRegion_create(void* start, size_t size)
{
    FreeRegionHeader_t* regionHeader = (FreeRegionHeader_t*) start;
    uintptr_t          footerAddr  = (uintptr_t)start + size - sizeof(AllocMetadata_t);
    AllocMetadata_t*   footer      = (AllocMetadata_t*) footerAddr;

    if (regionHeader == NULL)
    {
        return NULL;
    }

    regionHeader->metadata.used = 0;
    regionHeader->metadata.size = size;
    regionHeader->next          = NULL;
    regionHeader->previous      = NULL;

    *footer = regionHeader->metadata;

    return regionHeader;
}

/**
 * @brief FreeRegion_getSizeForAlignment Calculate the size of the free region to holds the payload
 *                                       in a way that the next free region payload become 16 byte aligned.
 * @param original                       The free region address
 * @param size                           Size of the payload to be allocated in this free region
 * @return                               Size of the free region aligned to 16 bytes
 *************************************************************************************************/
static size_t FreeRegion_getSizeForAlignment(FreeRegionHeader_t* original, size_t size)
{
    uintptr_t           originalAddr  = (uintptr_t)original;

    uint32_t            freeRegionMinimumSize     = sizeof(FreeRegionHeader_t) + sizeof(AllocMetadata_t);
    uint32_t            freeMetadataPaddingAmount = (size >= freeRegionMinimumSize) ? 0 : (freeRegionMinimumSize - (size % freeRegionMinimumSize));
    uint32_t            regionAlignment           = 16; // payload byte-alignment
    uint32_t            regionEndAddress          = originalAddr + size + freeMetadataPaddingAmount + sizeof(AllocMetadata_t);
    uint32_t            alignmentPaddingAmount    = regionAlignment - (regionEndAddress % regionAlignment);

    /* if the free region is too small for its metadata   *
     * padding to have space for the free region metadata */
    size = size + freeMetadataPaddingAmount;

    /* padding to align payload */
    size = size + alignmentPaddingAmount;

    return size;
}

/**
 * @brief FreeRegion_split  Split an free region into two.
 * @param original          Original region to be splitted in two.
 * @param size              Size of the original region after the split
 * @return                  The freshly new splitted region
 *************************************************************************************************/
static FreeRegionHeader_t* FreeRegion_split(FreeRegionHeader_t* original, size_t size)
{
    uintptr_t           originalAddr  = (uintptr_t)original;
    size_t              alignedSize = FreeRegion_getSizeForAlignment(original, size);

    uintptr_t           newFreeAddr = originalAddr + alignedSize;
    FreeRegionHeader_t* newFree     = (FreeRegionHeader_t*) newFreeAddr;
    uint32_t            newFreeSize = original->metadata.size - alignedSize;

    FreeRegion_create(original, alignedSize);

    /* If the "newFree" region calculated address corresponds to an allocated block
     * DO NOTHING OR BAD THINGS WILL HAPPEN! (user data corruption is the least bad of them) */
    if (newFree->metadata.used == 1)
    {
        return NULL;
    }

    /* If the new free region is to small to hold the own metadata, skip it */
    if (newFreeSize < (sizeof(FreeRegionHeader_t) + sizeof(AllocMetadata_t)))
    {
        return NULL;
    }

    /* Ok, we have a SAFE non-allocated region to work */
    FreeRegion_create(newFree, newFreeSize);

    return newFree;
}

/*************************************************************************************************/
/*********************************** Memory block methods ****************************************/

/**
 * @brief Block_coallesceBothSides Coallesce with left and right free regions
 * @param _this                    Heap block which the free regions belong
 * @param left                     Left free region footer
 * @param anchor                   Reference free region header
 * @param right                    Right free region header
 *************************************************************************************************/
static void Block_coallesceBothSides(BlockHeader_t* _this, AllocMetadata_t* left, FreeRegionHeader_t* reference, AllocMetadata_t* right)
{
    AllocMetadata_t* footer;
    FreeRegionHeader_t* leftFreeRegion  = (FreeRegionHeader_t*) ((uintptr_t)(reference) - left->size);
    FreeRegionHeader_t* rightFreeRegion = (FreeRegionHeader_t*) right;

    Block_removeRegionFromFreeList(_this, leftFreeRegion);   // Size will change
    Block_removeRegionFromFreeList(_this, rightFreeRegion);  // Will not exist anymore
    Block_removeRegionFromFreeList(_this, reference);        // Will not exist anymore

    leftFreeRegion->metadata.size += reference->metadata.size + rightFreeRegion->metadata.size;
    footer                    = (AllocMetadata_t*) ((uintptr_t)(leftFreeRegion) + leftFreeRegion->metadata.size - sizeof(AllocMetadata_t));
    footer->size              = leftFreeRegion->metadata.size;

    Block_addRegionToFreeList(_this, leftFreeRegion);             // New bigger size
}

/**
 * @brief Block_coallesceLeftSide Coallesce with left free region
 * @param _this                   Heap block which the free regions belong
 * @param left                    Left free region footer
 * @param anchor                  Reference free region header
 *************************************************************************************************/
static void Block_coallesceLeftSide(BlockHeader_t* _this, AllocMetadata_t* left, FreeRegionHeader_t* reference)
{
    AllocMetadata_t* footer;
    FreeRegionHeader_t* leftFreeRegion = (FreeRegionHeader_t*)( (uintptr_t)(reference) - left->size);

    Block_removeRegionFromFreeList(_this, leftFreeRegion);  // Size will change
    Block_removeRegionFromFreeList(_this, reference);       // Will not exist anymore

    leftFreeRegion->metadata.size   += reference->metadata.size;
    footer                           = (AllocMetadata_t*) ((uintptr_t)(leftFreeRegion) + leftFreeRegion->metadata.size - sizeof(AllocMetadata_t));
    footer->size                     = leftFreeRegion->metadata.size;

    Block_addRegionToFreeList(_this, leftFreeRegion);       // New bigger size
}

/**
 * @brief Block_coallesceRightSide Coallesce with right free region
 * @param _this                    Heap block which the free regions belong
 * @param anchor                   Reference free region header
 * @param right                    Right free region header
 *************************************************************************************************/
static void Block_coallesceRightSide(BlockHeader_t* _this, FreeRegionHeader_t* reference, AllocMetadata_t* right)
{
    AllocMetadata_t* footer;
    FreeRegionHeader_t* rightFreeRegion = (FreeRegionHeader_t*) right;

    Block_removeRegionFromFreeList(_this, rightFreeRegion);  // Will not exist anymore
    Block_removeRegionFromFreeList(_this, reference);        // Size will change

    reference->metadata.size += right->size;
    footer                    = (AllocMetadata_t*) ((uintptr_t)(reference) + reference->metadata.size - sizeof(AllocMetadata_t));
    footer->size              = reference->metadata.size;

    Block_addRegionToFreeList(_this, reference);             // New bigger size
}

/**
 * @brief Block_isFreeRegion Checks if the address belongs to a free region in this heap block
 * @param _this              Heap block to be searched
 * @param addr               The header address of the region
 * @return                   True (1) if found, false (0) otherwise
 *************************************************************************************************/
static uint32_t Block_isFreeRegion(BlockHeader_t* _this, AllocMetadata_t* addr)
{
    uint32_t i;
    FreeRegionHeader_t* it;

    for (i=0; i<FREE_BLOCKS_SETS; i++)
    {
        for(it = _this->freeRegions[i]; it != NULL; it = it->next)
        {
            uintptr_t headerAddr = (uintptr_t)it;
            uintptr_t footerAddr = headerAddr + it->metadata.size - sizeof(AllocMetadata_t);
            AllocMetadata_t* header = (AllocMetadata_t*)headerAddr;
            AllocMetadata_t* footer = (AllocMetadata_t*)footerAddr;

            if (header == addr || footer == addr)
            {
                return 1;
            }
        }
    }

    return 0;
}

/**
 * @brief Block_coallesceFreeRegion Try to coallesce the free region if neightbour free regions are found
 * @param _this                     The heap block which the free region belongs
 * @param freeRegion                The free region to be coallesced
 *************************************************************************************************/
static void Block_coallesceFreeRegion(BlockHeader_t* _this, FreeRegionHeader_t* freeRegion)
{
    uintptr_t           ptr        = (uintptr_t)freeRegion;
    AllocMetadata_t*    prevRegion = (AllocMetadata_t*)(ptr - sizeof(AllocMetadata_t));
    AllocMetadata_t*    nextRegion = (AllocMetadata_t*)(ptr + freeRegion->metadata.size);

    uint32_t            prevIsFree = Block_isFreeRegion(_this, prevRegion);
    uint32_t            nextIsFree = Block_isFreeRegion(_this, nextRegion);

    // CASE 2
    if (prevIsFree == 0 && nextIsFree == 1)
    {
        Block_coallesceRightSide(_this, freeRegion, nextRegion);
        return;
    }

    // CASE 3
    if (prevIsFree == 1 && nextIsFree == 0)
    {
        Block_coallesceLeftSide(_this, prevRegion, freeRegion);
        return;
    }

    // CASE 4
    if (prevIsFree == 1 && nextIsFree == 1)
    {
        Block_coallesceBothSides(_this, prevRegion, freeRegion, nextRegion);
        return;
    }

    // CASE 1
    //if (prevIsFree == 0 && nextIsFree == 0)
    return;
}

/**
 * @brief Block_addRegionToFreeList Add a free region to the free region list linked list
 * @param list                      The block heap which the linked list resides
 * @param item                      The item to be added
 * @return
 *************************************************************************************************/
static uint32_t Block_addRegionToFreeList(BlockHeader_t* _this, FreeRegionHeader_t* item)
{
    uint32_t             i;
    FreeRegionHeader_t** list;
    FreeRegionHeader_t*  aux;

    if (item == NULL)
    {
        return 0;
    }

    if (item->metadata.size == 0)
    {
        return 0;
    }

    i    = toFreeListIndex(item->metadata.size);
    list = &_this->freeRegions[i];
    aux  = *list;

    if (*list == NULL)
    {
        item->next     = NULL;
        item->previous = NULL;
        *list = item;
        return 0;
    }

    if (*list > item)
    {
        item->next        = *list;
        item->previous    = NULL;
        (*list)->previous = item;
        *list = item;
        return 0;
    }

    // Get the last item in list
    // Order by region address
    while ((aux->next != NULL) && (item < aux->next))
    {
        aux = aux->next;
    }

    item->next     = aux->next;
    item->previous = aux;
    aux->next      = item;

    return 0;
}

/**
 * @brief Block_removeRegionFromFreeList Remove a free region to the free region list linked list
 * @param list                           The block heap which the linked list resides
 * @param item                           The item to be remove
 * @return
 *************************************************************************************************/
static uint32_t Block_removeRegionFromFreeList(BlockHeader_t* _this, FreeRegionHeader_t* item)
{
    uint32_t             i    = toFreeListIndex(item->metadata.size);
    FreeRegionHeader_t** list = &_this->freeRegions[i];
    FreeRegionHeader_t* aux;

    if (item == NULL)
    {
        return -1;
    }

    // Case 1: first element of the list
    if (*list == item)
    {
        FreeRegionHeader_t* next = item->next;

        (*list) = next;

        if (next != NULL)
        {
            next->previous = NULL;
        }

        item->next      = NULL;
        item->previous  = NULL;

        return 0;
    }

    // Case 2: element in the middle of the list
    for (aux = *list; aux != NULL; aux = aux->next)
    {
        if (aux->next == item)
        {
            FreeRegionHeader_t* next = item->next;

            aux->next = next;

            if (next != NULL)
            {
                next->previous = aux;
            }

            break;
        }
    }

    item->next      = NULL;
    item->previous  = NULL;

    return 0;
}

/**
 * @brief Block_useRegion Change the metadata of a free region to it becomes a allocated region
 * @param _this           The block which the free region belongs
 * @param freeRegion      The free region to be allocated
 * @return                The allocated region
 *************************************************************************************************/
static AllocMetadata_t* Block_useRegion(BlockHeader_t* _this, FreeRegionHeader_t* freeRegion)
{
    uintptr_t        freeRegionAddr   = (uintptr_t)freeRegion;
    AllocMetadata_t* freeRegionFooter = (AllocMetadata_t*)(freeRegionAddr + freeRegion->metadata.size - sizeof(AllocMetadata_t));

    freeRegion->metadata.used = 1;
    freeRegionFooter->used    = 1;

    _this->usedSize += freeRegion->metadata.size;

    return (AllocMetadata_t*) freeRegionAddr;
}

/**
 * @brief Block_freeRegion Change the metadata of a allocated region to it becomes a free region
 * @param _this            The block which the allocated region belongs
 * @param region           The allocated region to be freed
 * @return                 The freed region
 *************************************************************************************************/
static FreeRegionHeader_t* Block_freeRegion(BlockHeader_t* _this, AllocMetadata_t* region)
{
    uintptr_t           regionAddr   = (uintptr_t)region;
    FreeRegionHeader_t* freeRegion   = (FreeRegionHeader_t*) regionAddr;
    AllocMetadata_t*    regionFooter = (AllocMetadata_t*)(regionAddr + region->size - sizeof(AllocMetadata_t));

    freeRegion->metadata.used = 0;
    freeRegion->next          = NULL;
    freeRegion->previous      = NULL;
    regionFooter->used  = 0;
    regionFooter->size  = freeRegion->metadata.size;

    _this->usedSize -= freeRegion->metadata.size;

    return freeRegion;
}

/**
 * @brief Block_create Allocate from kernel a some pages to use
 * @param size         Block size in bytes requested by user
 * @return             A block of memory which can hold at least data @param size bytes
 *************************************************************************************************/
static BlockHeader_t* Block_create(size_t size)
{
    BlockHeader_t*  blockHeader  = NULL;
    void*           memoryPtr    = NULL;
    size_t          memorySize   = size + sizeof(BlockHeader_t) + sizeof(FreeRegionHeader_t) + sizeof(AllocMetadata_t);
    size_t          pageQuantity = (memorySize / PAGE_SIZE) + (memorySize % PAGE_SIZE > 0);

    memoryPtr = libhalloc_alloc(pageQuantity);

    if (memoryPtr == NULL)
    {
        return NULL;
    }

    blockHeader = (BlockHeader_t*) memoryPtr;

    blockHeader->pages      = pageQuantity;
    blockHeader->size       = pageQuantity * PAGE_SIZE;
    blockHeader->next       = NULL; //blockHeader;  // Point to itself
    blockHeader->previous   = NULL; //blockHeader;  // Point to itself
    blockHeader->usedSize   = sizeof(BlockHeader_t);
    memset(blockHeader->freeRegions, 0, sizeof(FreeRegionHeader_t*) * FREE_BLOCKS_SETS);

    // Create the only free region covering the rest of the block
    blockHeader->freeRegions[LARGE_FREE_BLOCK_INDEX] = FreeRegion_create(memoryPtr + sizeof(BlockHeader_t), blockHeader->size - sizeof(BlockHeader_t));

    return blockHeader;
}

/**
 * @brief Block_isFull Informs if the heap block not contains any free region left
 * @param _this        Block to be checked
 * @return             True(1) if full or false(0) otherwise
 *************************************************************************************************/
static uint32_t Block_isFull(BlockHeader_t* _this)
{
    return (_this != NULL) && (_this->usedSize == _this->size);
}

/**
 * @brief Block_canAllocateSize Check if heap block can allocate the specified size
 * @param _this                 Heap block to be searched
 * @param size                  The payload size
 * @return                      The free region if found or null otherwise
 *************************************************************************************************/
static FreeRegionHeader_t* Block_canAllocateSize(BlockHeader_t* _this, uint32_t size)
{
    uint32_t i = 0;
    FreeRegionHeader_t* it;

    if (_this == NULL)
    {
        return NULL;
    }

    for (i = 0; i<FREE_BLOCKS_SETS; i++)
    {
        for (it = _this->freeRegions[i]; it != NULL; it = it->next)
        {
            size_t alignedSize = FreeRegion_getSizeForAlignment(it, size);

            if ( alignedSize < it->metadata.size )
            {
                return it;
            }
        }
    }

    return NULL;
}

/**
 * @brief Block_haveUserAllocations Informs if the heap block has any user allocations
 * @param _this                     The heap block
 * @return                          True(1) or false(0)
 *************************************************************************************************/
static uint32_t Block_haveUserAllocations(BlockHeader_t* _this)
{
    if (_this == NULL)
    {
        return 0;
    }

    return _this->usedSize > emptyBlockOverheadSize;
}

/**
 * @brief Block_allocateRegion Allocate a region for use
 * @param _this                Heap block to be used in allocation
 * @param size                 Size requested by user
 * @return                     Pointer to header of allocated region
 *************************************************************************************************/
static AllocMetadata_t* Block_allocateRegion(BlockHeader_t* _this, size_t size)
{
    uint32_t            regionSize    = PAYLOAD_WITH_OVERHEAD(size);
    FreeRegionHeader_t* newFreeRegion = NULL;
    FreeRegionHeader_t* freeRegion    = Block_canAllocateSize(_this, regionSize);

    if (freeRegion == NULL)
    {
        return NULL;
    }

    Block_removeRegionFromFreeList(_this, freeRegion);        // It will be allocated
    newFreeRegion = FreeRegion_split(freeRegion, regionSize); // Split
    Block_addRegionToFreeList( _this, newFreeRegion);         // New region to free list

    return Block_useRegion(_this, freeRegion); // Aloc
}

/**
 * @brief Block_deallocateRegion Deallocate a region in block
 * @param _this                  Heap block to be used
 * @param region                 Allocated region header
 * @return                       Pointer to header of freed region
 *************************************************************************************************/
static void Block_deallocateRegion(BlockHeader_t* _this, AllocMetadata_t* region)
{
   FreeRegionHeader_t* freedRegion = Block_freeRegion(_this, region);

   Block_addRegionToFreeList(_this, freedRegion); // New region come to free list
   Block_coallesceFreeRegion(_this, freedRegion); // Coallesce, if necessary

   return;
}

/*************************************************************************************************/
/*********************************** Public interface functions **********************************/

/**
 * @brief malloc
 * @param size
 * @return
 *************************************************************************************************/
void* malloc(size_t size)
{
    BlockHeader_t*   block     = NULL;
    AllocMetadata_t* memoryPtr = NULL;

    block = getBlockWithFreeRegion(size);

    memoryPtr = Block_allocateRegion(block, size);

    if (memoryPtr == NULL)
    {
        return 0;
    }

    return (void*)(memoryPtr) + sizeof(AllocMetadata_t);
}

/**
 * @brief realloc
 * @param pointer
 * @param size
 * @return
 *************************************************************************************************/
void* realloc(void* pointer, size_t size)
{
    AllocMetadata_t* headerPtr     = (AllocMetadata_t*)(pointer - sizeof(AllocMetadata_t));
    size_t           payloadLength = headerPtr->size - (sizeof(AllocMetadata_t)*2);
    size_t           copyLength    = payloadLength;
    void*            newMemoryPtr;

    if (pointer == NULL)
    {
        return malloc(size);
    }

    if (payloadLength == size)
    {
        return pointer;
    }

    if (size < payloadLength)
    {
        copyLength = size;
    }

    newMemoryPtr = malloc(size);

    if (newMemoryPtr == NULL)
    {
        return NULL;
    }

    memcpy(newMemoryPtr, pointer, copyLength);
    free (pointer);
    return newMemoryPtr;
}

/**
 * @brief calloc
 * @param num
 * @param size
 * @return
 *************************************************************************************************/
void* calloc(size_t num, size_t size)
{
    void*            memoryPtr;
    AllocMetadata_t* header;
    size_t           payloadLength;

    if (size == 0)
    {
        return NULL;
    }

    memoryPtr     = malloc(num*size);

    if (memoryPtr == NULL)
    {
        return NULL;
    }

    header        = (AllocMetadata_t*)(memoryPtr - sizeof(AllocMetadata_t));
    payloadLength = header->size - (sizeof(AllocMetadata_t)*2);

    memset(memoryPtr, 0, payloadLength);

    return memoryPtr;
}

/**
 * @brief free
 * @param pointer
 *************************************************************************************************/
void free(void* pointer)
{
    AllocMetadata_t* allocatedRegion = (AllocMetadata_t*) (pointer - sizeof(AllocMetadata_t));
    BlockHeader_t*   block           = NULL;

    if (allocatedRegion->used == 0)
    {
        return;
    }

    block = getBlockWithRegion(pointer);

    if (block == NULL)
    {
        return; // Error
    }

    Block_deallocateRegion(block, allocatedRegion);

    // If the block does not contains user Allocations
    // return it to the kernel
    if (Block_haveUserAllocations(block) == 0)
    {
        BlockList_removeBlockFromList(&blockList, block);
        libhalloc_free(block, block->pages);
    }
}

/**
 * @brief mallocstats
 *************************************************************************************************/
void mallocstats()
{
    BlockHeader_t*   block = NULL;
    uint32_t i;

    for (block = blockList, i = 0; block != NULL; block = block->next, i++)
    {
        uint32_t j;
        uint32_t freeRegionsCount       = 0;
        uint32_t freeHeapSpace          = 0;
        uint32_t largestFreeRegionSize  = 0;
        uint32_t smallestFreeRegionSize = UINT32_MAX;

        for(j = 0; j<FREE_BLOCKS_SETS; j++)
        {
            FreeRegionHeader_t* it;

            for (it = block->freeRegions[j]; it != NULL; it = it->next)
            {
                freeRegionsCount++;
                freeHeapSpace += it->metadata.size;

                if (it->metadata.size > largestFreeRegionSize)
                {
                    largestFreeRegionSize = it->metadata.size;
                }

                if (it->metadata.size < smallestFreeRegionSize)
                {
                    smallestFreeRegionSize = it->metadata.size;
                }
            }
        }

        printf("Block[%d] (Start Addr: %.x):\n", i, block);
        printf("  Pages (allocated from kernel) : %d\n", block->pages);
        printf("  Size  (allocated from kernel) : %d bytes\n", block->size);
        printf("  Used Size (allocated to app)  : %d bytes\n", block->usedSize);
        printf("  Free statistics:\n");
        printf("    Free Regions Count : %d\n", freeRegionsCount);
        printf("    Largest Free Space : %d bytes\n", largestFreeRegionSize);
        printf("    Smallest Free Space: %d bytes\n", smallestFreeRegionSize);
        printf("    Free Heap Space    : %d bytes\n", freeHeapSpace);

        for(j = 0; j<FREE_BLOCKS_SETS; j++)
        {
            FreeRegionHeader_t* it;
            printf("      FreeRegion[%d]: ", j);
            for (it = block->freeRegions[j]; it != NULL; it = it->next)
            {
                printf("%#x (%d bytes)", it, it->metadata.size);
            }
            printf("\n");
        }
    }
}
