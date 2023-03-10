/**
 * \file
 * \brief Local memory allocator for init till mem_serv is ready to use
 */

#include "mem_alloc.h"
#include <mm/mm.h>
#include <aos/paging.h>
#include <grading.h>

/// MM allocator instance data
struct mm aos_mm;
char buf[64000];

errval_t aos_ram_alloc_aligned(struct capref *ret, size_t size, size_t alignment)
{
    return mm_alloc_aligned(&aos_mm, size, alignment, ret);
}

errval_t aos_ram_free(struct capref cap)
{
    return mm_free(&aos_mm, cap);
}

static inline errval_t initialize_ram_allocator(void)
{
    errval_t err;

    // Init slot allocator
    static struct slot_prealloc init_slot_alloc;
    struct capref cnode_cap = {
        .cnode = {
            .croot = CPTR_ROOTCN,
            .cnode = ROOTCN_SLOT_ADDR(ROOTCN_SLOT_SLOT_ALLOC0),
            .level = CNODE_TYPE_OTHER,
        },
        .slot = 0,
    };
    err = slot_prealloc_init(&init_slot_alloc, cnode_cap, L2_CNODE_SLOTS, &aos_mm);
    if (err_is_fail(err)) {
        return err_push(err, MM_ERR_SLOT_INIT);
    }

    // Initialize aos_mm
    err = mm_init(&aos_mm, ObjType_RAM, NULL,
                  slot_alloc_prealloc, slot_prealloc_refill,
                  &init_slot_alloc);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "Can't initalize the memory manager.");
    }

    // Give aos_mm a bit of memory for the initialization
    // M1 TODO: grow be with some memory!
    slab_grow(&aos_mm.slabs, &buf, 64000);

    return SYS_ERR_OK;
}

/**
 * \brief Setups a local memory allocator for init to use till the memory server
 * is ready to be used. Inspects bootinfo for finding memory region.
 */
errval_t initialize_ram_alloc(void)
{
    errval_t err;

    err = initialize_ram_allocator();
    if (err_is_fail(err)) {
        return err;
    }

    // Walk bootinfo and add all RAM caps to allocator handed to us by the kernel
    uint64_t mem_avail = 0;
    struct capref mem_cap = {
        .cnode = cnode_super,
        .slot = 0,
    };

    for (int i = 0; i < bi->regions_length; i++) {
        if (bi->regions[i].mr_type == RegionType_Empty && !(bi->regions[i].mr_consumed)) {
            struct capability c;
            err = cap_direct_identify(mem_cap, &c);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "failed to get the frame info\n");
            }

            // some santity checks
            assert(c.type == ObjType_RAM);
            assert(c.u.ram.base == bi->regions[i].mr_base);
            assert(c.u.ram.bytes == bi->regions[i].mr_bytes);

            err = mm_add(&aos_mm, mem_cap);
            if (err_is_ok(err)) {
                mem_avail += bi->regions[i].mr_bytes;
            } else {
                DEBUG_ERR(err, "Warning: adding RAM region %d (%p/%zu) FAILED", i, bi->regions[i].mr_base, bi->regions[i].mr_bytes);
            }

            bi->regions[i].mr_consumed = true;
            mem_cap.slot++;
        }
    }
    debug_printf("Added %"PRIu64" MB of physical memory.\n", mem_avail / 1024 / 1024);

    // Finally, we can initialize the generic RAM allocator to use our local allocator
    err = ram_alloc_set(aos_ram_alloc_aligned);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_RAM_ALLOC_SET);
    }
/*
    printf("dasdasd2\n");
    struct capref garbage;
    for (int i = 0; i < 5000; i++) {
        err = mm_alloc(&aos_mm, 1024 * 3, &garbage);
        if (err_is_ok(err)) debug_printf("Added %i\n", i);
        else {
            DEBUG_ERR(err, "Warning: Allocating failed %i", i);
            while (1);
        }
        err = mm_free(&aos_mm, garbage);
        if (err_is_ok(err)) debug_printf("Removed %i\n", i);
        else {
            DEBUG_ERR(err, "Warning: Allocating failed %i", i);
            while (1);
        }
    }

    struct capref arr[1800];
    for (int i = 0; i < 1800; i++) {
        err = mm_alloc(&aos_mm, 1024 * 1024, &arr[i]);
        if (err_is_ok(err)) debug_printf("Added %i\n", i);
        else {
            DEBUG_ERR(err, "Warning: Allocating failed %i", i);
            while (1);
        }
    }
    for (int i = 0; i < 1800; i++) {
        printf(".\n");
        err = mm_free(&aos_mm, arr[i]);
        printf(".\n");
        if (err_is_ok(err)) debug_printf("Removed %i\n", i);
        else {
        printf("ERR\n");
            DEBUG_ERR(err, "Warning: Allocating failed %i", i);
            while (1);
        }
    }
    for (int i = 0; i < 450; i++) {
        err = mm_alloc(&aos_mm, 1024 * 1024 * 4, &arr[i]);
        if (err_is_ok(err)) debug_printf("Added %i\n", i);
        else {
            DEBUG_ERR(err, "Warning: Allocating failed %i", i);
            while (1);
        }
    }
    for (int i = 0; i < 450; i++) {
        err = mm_free(&aos_mm, arr[i]);
        if (err_is_ok(err)) debug_printf("Removed %i\n", i);
        else {
            DEBUG_ERR(err, "Warning: Allocating failed %i", i);
            while (1);
        }
    }

*/


    //struct capref cap;
    // paging_map_fixed_attr(get_current_paging_state(), 0x0005550000000000,
    //                            cap , 4096, 0);

    // Grading
    grading_test_mm(&aos_mm);

    return SYS_ERR_OK;
}

