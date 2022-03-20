/**
 * \file
 * \brief AOS paging helpers.
 */

/*
 * Copyright (c) 2012, 2013, 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstr. 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <aos/aos.h>
#include <aos/paging.h>
#include <aos/except.h>
#include <aos/slab.h>
#include "threads_priv.h"

#include <stdio.h>
#include <string.h>

static struct paging_state current;

#define SLAB_INIT_BUF_LEN 262144 // for starting out, 256kB should be enough for the memory manager to begin mapping some pages
static char slab_init_buf[SLAB_INIT_BUF_LEN];

/**
 * \brief Helper function that allocates a slot and
 *        creates a aarch64 page table capability for a certain level
 */
static errval_t pt_alloc(struct paging_state * st, enum objtype type, 
                         struct capref *ret) 
{
    errval_t err;
    err = st->slot_alloc->alloc(st->slot_alloc, ret);
    if (err_is_fail(err)) {
        debug_printf("slot_alloc failed: %s\n", err_getstring(err));
        return err;
    }
    err = vnode_create(*ret, type);
    if (err_is_fail(err)) {
        debug_printf("vnode_create failed: %s\n", err_getstring(err));
        return err;
    }
    return SYS_ERR_OK;
}

__attribute__((unused)) static errval_t pt_alloc_l1(struct paging_state * st, struct capref *ret)
{
    return pt_alloc(st, ObjType_VNode_AARCH64_l1, ret);
}

__attribute__((unused)) static errval_t pt_alloc_l2(struct paging_state * st, struct capref *ret)
{
    return pt_alloc(st, ObjType_VNode_AARCH64_l2, ret);
}

__attribute__((unused)) static errval_t pt_alloc_l3(struct paging_state * st, struct capref *ret) 
{
    return pt_alloc(st, ObjType_VNode_AARCH64_l3, ret);
}


/**
 * TODO(M2): Implement this function.
 * TODO(M4): Improve this function.
 * \brief Initialize the paging_state struct for the paging
 *        state of the calling process.
 * 
 * \param st The struct to be initialized, must not be NULL.
 * \param start_vaddr Virtual address allocation should start at
 *        this address.
 * \param pdir Reference to the cap of the L0 VNode.
 * \param ca The slot_allocator to be used by the paging state.
 * \return Either SYS_ERR_OK if no error occured or an error
 * indicating what went wrong otherwise.
 */
errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
                           struct capref pdir, struct slot_allocator *ca)
{
    // TODO (M2): Implement state struct initialization
    // TODO (M4): Implement page fault handler that installs frames when a page fault
    // occurs and keeps track of the virtual address space.
    return LIB_ERR_NOT_IMPLEMENTED;
}

/**
 * TODO(M2): Implement this function.
 * TODO(M4): Improve this function.
 * \brief Initialize the paging_state struct for the paging state
 *        of a child process.
 * 
 * \param st The struct to be initialized, must not be NULL.
 * \param start_vaddr Virtual address allocation should start at
 *        this address.
 * \param pdir Reference to the cap of the L0 VNode.
 * \param ca The slot_allocator to be used by the paging state.
 * \return Either SYS_ERR_OK if no error occured or an error
 * indicating what went wrong otherwise.
 */
errval_t paging_init_state_foreign(struct paging_state *st, lvaddr_t start_vaddr,
                           struct capref pdir, struct slot_allocator *ca)
{
    // TODO (M2): Implement state struct initialization
    // TODO (M4): Implement page fault handler that installs frames when a page fault
    // occurs and keeps track of the virtual address space.
    return LIB_ERR_NOT_IMPLEMENTED;
}

/**
 * @brief This function initializes the paging for this domain
 *
 * Note: The function is called once before main.
 */
errval_t paging_init(void)
{
    debug_printf("paging_init\n");
    // TODO (M2): Call paging_init_state for &current
    // TODO (M4): initialize self-paging handler
    // TIP: use thread_set_exception_handler() to setup a page fault handler
    // TIP: Think about the fact that later on, you'll have to make sure that
    // you can handle page faults in any thread of a domain.
    // TIP: it might be a good idea to call paging_init_state() from here to
    // avoid code duplication.
	
	//errval_t err;
	//err = slot_alloc_init();
	//DEBUG_ERR(err, "slot_alloc_init");
	
	//err = two_level_slot_alloc_init(&msa);
	//if (err_is_fail(err)) {
	//	USER_PANIC_ERR(err, "Failed to init slot_alloc");
	//	return err;
	//}
	//current.slot_alloc = &(msa.a);
	
	current.slot_alloc = get_default_slot_allocator();
	
	current.root_page_tbl.cap = cap_vroot;
	current.root_page_tbl.first = NULL;
	current.root_page_tbl.last = NULL;
	
	
	slab_init(&(current.slab_alloc), sizeof(struct mm_l1_vnode_meta), slab_default_refill);
	slab_grow(&(current.slab_alloc), slab_init_buf, SLAB_INIT_BUF_LEN);
	
    set_current_paging_state(&current);
    return SYS_ERR_OK;
}


/**
 * @brief Initializes the paging functionality for the calling thread
 *
 * @param[in] t   the tread to initialize the paging state for.
 *
 * This function prepares the thread to handing its own page faults
 */
errval_t paging_init_onthread(struct thread *t)
{
    // TODO (M4):
    //   - setup exception handler for thread `t'.
    return LIB_ERR_NOT_IMPLEMENTED;
}



/**
 * @brief Find a free region of virtual address space that is large enough to accomodate a
 *        buffer of size 'bytes'.
 *
 * @param[in]  st          A pointer to the paging state to allocate from
 * @param[out] buf         Returns the free virtual address that was found.
 * @param[in]  bytes       The requested (minimum) size of the region to allocate
 * @param[in]  alignment   The address needs to be a multiple of 'alignment'.
 *
 * @return Either SYS_ERR_OK if no error occured or an error indicating what went wrong otherwise.
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes, size_t alignment)
{
    /**
     * TODO(M2): Implement this function
     *   - Find a region of free virtual address space that is large enough to
     *     accomodate a buffer of size `bytes`.
     */
    *buf = NULL;

    return LIB_ERR_NOT_IMPLEMENTED;
}


/**
 * \brief Finds a free virtual address and maps `bytes` of the supplied frame at that address
 *
 * @param[in]  st      the paging state to create the mapping in
 * @param[out] buf     returns the virtual address at which this frame has been mapped.
 * @param[in]  bytes   the number of bytes to map.
 * @param[in]  frame   the frame capability to be mapped
 * @param[in]  flags   The flags that are to be set for the newly mapped region,
 *                     see 'paging_flags_t' in paging_types.h .
 *
 * @return Either SYS_ERR_OK if no error occured or an error indicating what went wrong otherwise.
 */
errval_t paging_map_frame_attr(struct paging_state *st, void **buf, size_t bytes,
                               struct capref frame, int flags)
{
    // TODO(M2):
    // - Find and allocate free region of virtual address space of at least bytes in size.
    // - Map the user provided frame at the free virtual address
    // - return the virtual address in the buf parameter
    //
    // Hint:
    //  - think about what mapping configurations are actually possible

    return LIB_ERR_NOT_IMPLEMENTED;
}


/**
 * @brief mapps the provided frame at the supplied address in the paging state
 *
 * @param[in] st      the paging state to create the mapping in
 * @param[in] vaddr   the virtual address to create the mapping at
 * @param[in] frame   the frame to map in
 * @param[in] bytes   the number of bytes that will be mapped.
 * @param[in] flags   The flags that are to be set for the newly mapped region,
 *                    see 'paging_flags_t' in paging_types.h .
 *
 * @return SYS_ERR_OK on success.
 */
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
                               struct capref frame, size_t bytes, int flags)
{
    /*
     * TODO(M1):
     *    - Map a frame assuming all mappings will fit into one leaf page table (L3)
     * TODO(M2):
     *    - General case: you will need to handle mappings spanning multiple leaf page tables.
     *    - Make sure to update your paging state to reflect the newly mapped region
     *
     * Hint:
     *  - think about what mapping configurations are actually possible
     */
	
	static int slab_refilling = 0;
	if (!slab_refilling && slab_freecount(&(st->slab_alloc)) < 64) {
		slab_refilling = 1;
		debug_printf("Refilling Paging slabs...");
		errval_t e = slab_default_refill(&(st->slab_alloc));
		debug_printf("Slab Refilling error: %d\n", err_no(e));
		slab_refilling = 1;
	}
	
	// calculate the slots necessary for this mapping
	capaddr_t l0_slot, l1_slot, l2_slot, l3_slot;
	l0_slot = (0x0000ff8000000000 & vaddr) >> 39;
	l1_slot = (0x0000007fc0000000 & vaddr) >> 30;
	l2_slot = (0x000000003fe00000 & vaddr) >> 21;
	l3_slot = (0x00000000001ff000 & vaddr) >> 12;
	
	//debug_printf("Default Slot Alloc Space: %d, NSlots: %d\n", get_default_slot_allocator()->space, get_default_slot_allocator()->nslots);
	
	struct capref l1_cap, l2_cap, l3_cap;
	
	errval_t err;
	
	struct mm_l1_vnode_meta *l1_meta = find_l1_vnode_meta(&(st->root_page_tbl), l0_slot);
	if (l1_meta) l1_cap = l1_meta->cap;
	else {
		// create new capability for the page table
		err = pt_alloc_l1(st, &l1_cap);
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "L1 page alloc");
			err = err_push(err, LIB_ERR_PMAP_ALLOC_VNODE);
			return err;
		}
		
		// insert meta info into datastructure
		l1_meta = slab_alloc(&(st->slab_alloc));
		l1_meta->cap = l1_cap;
		l1_meta->slot = l0_slot;
		
		l1_meta->next = NULL;
		l1_meta->prev = NULL;
		
		l1_meta->first = NULL;
		l1_meta->last = NULL;
		
		if (st->root_page_tbl.last) st->root_page_tbl.last->next = l1_meta;
		l1_meta->prev = st->root_page_tbl.last;
		st->root_page_tbl.last = l1_meta;
		if (!st->root_page_tbl.first) st->root_page_tbl.first = l1_meta;
		
		// map page table entry
		st->slot_alloc->alloc(st->slot_alloc, &(l1_meta->map));
		err = vnode_map(current.root_page_tbl.cap, l1_cap, l0_slot, flags, 0, 1, l1_meta->map); // TODO: find appropriate flags
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "L1 Page mapping");
			err = err_push(err, LIB_ERR_VNODE_MAP);
			return err;
		}
	}
	
	struct mm_l2_vnode_meta *l2_meta = find_l2_vnode_meta(l1_meta, l1_slot);
	if (l2_meta) l2_cap = l2_meta->cap;
	else {
		// create new capability for the page table
		err = pt_alloc_l2(st, &l2_cap);
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "L2 page alloc");
			err = err_push(err, LIB_ERR_PMAP_ALLOC_VNODE);
			return err;
		}
		
		// insert meta info into datastructure
		l2_meta = slab_alloc(&(st->slab_alloc));
		l2_meta->cap = l2_cap;
		l2_meta->slot = l1_slot;
		
		l2_meta->next = NULL;
		l2_meta->prev = NULL;
		
		l2_meta->first = NULL;
		l2_meta->last = NULL;
		
		if (l1_meta->last) l1_meta->last->next = l2_meta;
		l2_meta->prev = l1_meta->last;
		l1_meta->last = l2_meta;
		if (!l1_meta->first) l1_meta->first = l2_meta;
		
		// map page table entry
		st->slot_alloc->alloc(st->slot_alloc, &(l2_meta->map));
		err = vnode_map(l1_meta->cap, l2_cap, l1_slot, flags, 0, 1, l2_meta->map);
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "L2 Page mapping");
			err = err_push(err, LIB_ERR_VNODE_MAP);
			return err;
		}
	}
	
	struct mm_l3_vnode_meta *l3_meta = find_l3_vnode_meta(l2_meta, l2_slot);
	if (l3_meta) l3_cap = l3_meta->cap;
	else {
		// create new capability for the page table
		err = pt_alloc_l3(st, &l3_cap);
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "L3 page alloc");
			err = err_push(err, LIB_ERR_PMAP_ALLOC_VNODE);
			return err;
		}
		
		// insert meta info into datastructure
		l3_meta = slab_alloc(&(st->slab_alloc));
		l3_meta->cap = l3_cap;
		l3_meta->slot = l2_slot;
		
		l3_meta->next = NULL;
		l3_meta->prev = NULL;
		
		l3_meta->first = NULL;
		l3_meta->last = NULL;
		
		if (l2_meta->last) l2_meta->last->next = l3_meta;
		l3_meta->prev = l2_meta->last;
		l2_meta->last = l3_meta;
		if (!l2_meta->first) l2_meta->first = l3_meta;
		
		// map page table entry
		st->slot_alloc->alloc(st->slot_alloc, &(l3_meta->map));
		err = vnode_map(l2_meta->cap, l3_cap, l2_slot, flags, 0, 1, l3_meta->map); // TODO: find appropriate flags
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "L3 Page mapping");
			err = err_push(err, LIB_ERR_VNODE_MAP);
			return err;
		}
	}
	
	
	struct mm_page_meta *page = find_page_meta(l3_meta, l3_slot);
	if (page) {
		// this page has already been mapped
		return LIB_ERR_PMAP_NOT_MAPPED;
	} else {
		page = slab_alloc(&(st->slab_alloc));
		page->slot = l3_slot;
		
		page->next = NULL;
		page->prev = NULL;
		
		st->slot_alloc->alloc(st->slot_alloc, &(page->map));
		err = vnode_map(l3_meta->cap, frame, l3_slot, flags, 0, (bytes / BASE_PAGE_SIZE) + (bytes % BASE_PAGE_SIZE ? 1 : 0), page->map);
		if (err_is_fail(err)) {
			DEBUG_ERR(err, "Page mapping");
			err = err_push(err, LIB_ERR_VREGION_MAP);
			return err;
		}
	}
	
	//debug_printf("Mapped Virtual Address: %p\n", vaddr);
	
    return SYS_ERR_OK;
}


/**
 * @brief Unmaps the region starting at the supplied pointer.
 *
 * @param[in] st      the paging state to create the mapping in
 * @param[in] region  starting address of the region to unmap
 *
 * @return SYS_ERR_OK on success, or error code indicating the kind of failure
 *
 * The supplied `region` must be the start of a previously mapped frame.
 *
 * @NOTE: Implementing this function is optional.
 */
errval_t paging_unmap(struct paging_state *st, const void *region)
{
    return LIB_ERR_NOT_IMPLEMENTED;
}
