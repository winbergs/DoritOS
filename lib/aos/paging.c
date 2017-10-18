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

#define PRINT_DEBUG 0

static errval_t delete_vspace_alloc_node(struct paging_state *st, lvaddr_t base, struct vspace_node **ret_node);
static errval_t insert_vspace_free_node(struct paging_state *st, struct vspace_node *new_node);


static struct paging_state current;

/**
 * \brief Helper function that allocates a slot and
 *        creates a ARM l2 page table capability
 */
static errval_t arml2_alloc(struct paging_state * st, struct capref *ret)
{
    errval_t err;
    err = st->slot_alloc->alloc(st->slot_alloc, ret);
    if (err_is_fail(err)) {
        debug_printf("slot_alloc failed: %s\n", err_getstring(err));
        return err;
    }
    err = vnode_create(*ret, ObjType_VNode_ARM_l2);
    if (err_is_fail(err)) {
        debug_printf("vnode_create failed: %s\n", err_getstring(err));
        return err;
    }
    return SYS_ERR_OK;
}

errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
        struct capref pdir, struct slot_allocator * ca)
{
    debug_printf("paging_init_state\n");
    // TODO (M4): Implement page fault handler that installs frames when a page fault
    // occurs and keeps track of the virtual address space.
    
    
    // Storing the reference to the slot allocator
    st->slot_alloc = ca;
    
    // Set the capability reference for the l1 page table
    st->l1_pagetable = pdir;
    
    // Initialize L2 page table tree
    st->l2_tree_root = NULL;
    
    // Set up state for vspace allocation
    st->free_vspace_head = NULL;
    st->alloc_vspace_head = NULL;
    st->free_vspace_base = start_vaddr;

    // Initialize the slab allocator for free vspace nodes
    st->vspace_slabs_prevent_refill = 0;
    slab_init(&st->vspace_slabs, sizeof(struct vspace_node), slab_default_refill);
    static int first_call = 1;
    if (first_call) {
        static char vspace_nodebuf[sizeof(struct vspace_node)*64];
        slab_grow(&st->vspace_slabs, vspace_nodebuf, sizeof(vspace_nodebuf));
    }
    else {
        slab_default_refill(&st->vspace_slabs);
    }
    
    // Initialize the slab allocator for tree nodes
    st->slabs_prevent_refill = 0;
    slab_init(&st->slabs, sizeof(struct pt_cap_tree_node), slab_default_refill);
    if (first_call) {
        static char nodebuf[sizeof(struct pt_cap_tree_node)*64];
        slab_grow(&st->slabs, nodebuf, sizeof(nodebuf));
    }
    else {
        slab_default_refill(&st->slabs);
    }
    
    first_call = 0;
    
    return SYS_ERR_OK;
}

/**
 * \brief This function initializes the paging for this domain
 * It is called once before main.
 */
errval_t paging_init(void)
{
    debug_printf("paging_init\n");
    // TODO (M4): initialize self-paging handler
    // TIP: use thread_set_exception_handler() to setup a page fault handler
    // TIP: Think about the fact that later on, you'll have to make sure that
    // you can handle page faults in any thread of a domain.
    // TIP: it might be a good idea to call paging_init_state() from here to
    // avoid code duplication.
    set_current_paging_state(&current);
    
    // Create the capability reference for the l1 page table at the default location in capability space
    struct capref pdir = {
        .cnode = cnode_page,
        .slot = 0
    };
    
    return paging_init_state(&current,
                             VADDR_OFFSET,
                             pdir,
                             get_default_slot_allocator());
}


/**
 * \brief Initialize per-thread paging state
 */
void paging_init_onthread(struct thread *t)
{
    // TODO (M4): setup exception handler for thread `t'.
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_init(struct paging_state *st, struct paging_region *pr, size_t size)
{
    void *base;
    errval_t err = paging_alloc(st, &base, size);
    if (err_is_fail(err)) {
        debug_printf("paging_region_init: paging_alloc failed\n");
        return err_push(err, LIB_ERR_VSPACE_MMU_AWARE_INIT);
    }
    pr->base_addr    = (lvaddr_t)base;
    pr->current_addr = pr->base_addr;
    pr->region_size  = size;
    pr->paging_state = st;
    return SYS_ERR_OK;
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_map(struct paging_region *pr, size_t req_size,
                           void **retbuf, size_t *ret_size)
{
    errval_t err = SYS_ERR_OK;
    
    // Round up to next page boundary
    if (req_size % BASE_PAGE_SIZE) {
        size_t pages = req_size / BASE_PAGE_SIZE;
        pages++;
        req_size = pages * BASE_PAGE_SIZE;
    }
    
    lvaddr_t end_addr = pr->base_addr + pr->region_size;
    ssize_t rem = end_addr - pr->current_addr;
    if (rem > req_size) {
        // ok
        *retbuf = (void*)pr->current_addr;
        *ret_size = req_size;
        pr->current_addr += req_size;
    } else if (rem > 0) {
        *retbuf = (void*)pr->current_addr;
        *ret_size = rem;
        pr->current_addr += rem;
        debug_printf("exhausted paging region, "
                "expect badness on next allocation\n");
    } else {
        return LIB_ERR_VSPACE_MMU_AWARE_NO_SPACE;
    }
    
    // Allocating frame
    struct capref frame_cap;
    err = frame_alloc(&frame_cap, *ret_size, ret_size);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Mapping frame into virtual memory
    err = paging_map_fixed(pr->paging_state, (lvaddr_t) *retbuf, frame_cap, *ret_size);
    
    return SYS_ERR_OK;
}

/**
 * \brief free a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 * NOTE: Implementing this function is optional.
 */
errval_t paging_region_unmap(struct paging_region *pr, lvaddr_t base, size_t bytes)
{
    // TIP: you will need to keep track of possible holes in the region
    
    
    
    return SYS_ERR_OK;
}

/**
 * \brief Allocate a fixed area in the virtual address space.
 */
errval_t paging_alloc_fixed(struct paging_state *st, void *buf, size_t bytes)
{
    
    // Check that the free list is empty
    //  FIXME: Maybe support changing the free list
    assert(st->free_vspace_head == NULL);
    
    // Check page alignment
    assert(!((lvaddr_t) buf % BASE_PAGE_SIZE));
    
    // Round up size to next page boundary
    if (bytes % BASE_PAGE_SIZE) {
        size_t pages = bytes / BASE_PAGE_SIZE;
        pages++;
        bytes = pages * BASE_PAGE_SIZE;
    }
    
    // Check that the virtual address range can be put into the allocated list
    assert((lvaddr_t) buf + bytes < st->free_vspace_base);
    
    // Register the allocation in the alloc list
    struct vspace_node *new_node = slab_alloc(&st->vspace_slabs);
    new_node->base = (uintptr_t) buf;
    new_node->base = bytes;
    new_node->next = st->alloc_vspace_head;
    st->alloc_vspace_head = new_node;
    
    // Check that there are sufficient slabs left in the slab allocator
    size_t freecount = slab_freecount((struct slab_allocator *)&st->vspace_slabs);
    if (freecount <= 6 && !st->vspace_slabs_prevent_refill) {
#if PRINT_DEBUG
        debug_printf("Vspace slab allocator refilling...\n");
#endif
        st->vspace_slabs_prevent_refill = 1;
        slab_default_refill((struct slab_allocator *)&st->vspace_slabs);
        st->vspace_slabs_prevent_refill = 0;
    }
    
    return SYS_ERR_OK;
    
}

/**
 *
 * \brief Find a bit of free virtual address space that is large enough to
 *        accomodate a buffer of size `bytes`.
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes)
{
    
#if PRINT_DEBUG
    debug_printf("Allocating %zu bytes of virtual address space...\n", bytes);
#endif
    
    // Round up to next page boundary
    if (bytes % BASE_PAGE_SIZE) {
        size_t pages = bytes / BASE_PAGE_SIZE;
        pages++;
        bytes = pages * BASE_PAGE_SIZE;
    }
    
    // Iterate free list and check for suitable address range
    struct vspace_node **indirect = &st->free_vspace_head;
    while ((*indirect) != NULL) {
        if ((*indirect)->size >= bytes) {
            break;
        }
        indirect = &(*indirect)->next;
    }
    
    // Check if we found a free address range
    if (*indirect) {
        // Return the base address of the node
        *buf = (void *) (*indirect)->base;
        // Check if free range needs to be split
        if ((*indirect)->size > bytes) {
            // Reconfigure the node
            (*indirect)->base += bytes;
            (*indirect)->size -= bytes;
        }
        else {
            // Remove the node
            *indirect = (*indirect)->next;
            // Free the slab
            slab_free(&st->vspace_slabs, *indirect);
        }
    }
    else {
        // Alocate at the end of the currently managed address range
        *buf = (void *) st->free_vspace_base;
        st->free_vspace_base += bytes;
    }
    
    // Register the allocation in the alloc list
    struct vspace_node *new_node = slab_alloc(&st->vspace_slabs);
    new_node->base = (uintptr_t) *buf;
    new_node->base = bytes;
    new_node->next = st->alloc_vspace_head;
    st->alloc_vspace_head = new_node;
    
    // Check that there are sufficient slabs left in the slab allocator
    size_t freecount = slab_freecount((struct slab_allocator *)&st->vspace_slabs);
    if (freecount <= 6 && !st->vspace_slabs_prevent_refill) {
#if PRINT_DEBUG
        debug_printf("Vspace slab allocator refilling...\n");
#endif
        st->vspace_slabs_prevent_refill = 1;
        slab_default_refill((struct slab_allocator *)&st->vspace_slabs);
        st->vspace_slabs_prevent_refill = 0;
    }
    
    // Summary
#if PRINT_DEBUG
    debug_printf("Allocated %zu bytes of virtual address space at 0x%x\n", bytes, *buf);
#endif
    
    return SYS_ERR_OK;
}

/**
 * \brief map a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 */
errval_t paging_map_frame_attr(struct paging_state *st, void **buf,
                               size_t bytes, struct capref frame,
                               int flags, void *arg1, void *arg2)
{
    errval_t err = paging_alloc(st, buf, bytes);
    if (err_is_fail(err)) {
        return err;
    }
    return paging_map_fixed_attr(st, (lvaddr_t)(*buf), frame, bytes, flags);
}

errval_t slab_refill_no_pagefault(struct slab_allocator *slabs, struct capref frame, size_t minbytes)
{
    // Refill the two-level slot allocator without causing a page-fault
    
    // Free the capability slot that was given to us, as we don't use it.
    slot_free(frame);
    
    // FIXME: Currently a full page is allocated. More is not supported.
    assert(minbytes <= BASE_PAGE_SIZE);
    
    // Perform the refill. FIXME: This should not cause a page fault. Hopefully.
    slab_default_refill(slabs);
    
    return SYS_ERR_OK;
}

/**
 * \brief map a user provided frame at user provided VA.
 * TODO(M1): Map a frame assuming all mappings will fit into one L2 pt
 * TODO(M2): General case 
 */
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
        struct capref frame, size_t bytes, int flags)
{
    
#if PRINT_DEBUG
    debug_printf("Mapping %d page(s) at 0x%x\n", bytes / BASE_PAGE_SIZE + (bytes % BASE_PAGE_SIZE ? 1 : 0), vaddr);
#endif
    
    for (uintptr_t end_addr, addr = vaddr; addr < vaddr + bytes; addr = end_addr) {
    
        // Find next boundary of L2 page table range
        end_addr = addr / (ARM_L2_MAX_ENTRIES * BASE_PAGE_SIZE);
        end_addr++;
        end_addr *= (ARM_L2_MAX_ENTRIES * BASE_PAGE_SIZE);
        
        // Calculate size of region to map within this L2 page table
        size_t size = MIN(end_addr - addr, (vaddr + bytes) - addr);
        
        
        // Calculate the offsets for the given virtual address
        uintptr_t l1_offset = ARM_L1_OFFSET(addr);
        uintptr_t l2_offset = ARM_L2_OFFSET(addr);
        uintptr_t mapping_offset = addr / BASE_PAGE_SIZE;

        // Search for L2 pagetable capability in the tree
        struct pt_cap_tree_node *node = st->l2_tree_root;
        struct pt_cap_tree_node *prev = node;

        while (node != NULL) {
            if (l1_offset == node->offset) {
                break;
            }
            prev = node;
            if (l1_offset < node->offset) {
                node = node->left;
            }
            else if (l1_offset > node->offset) {
                node = node->right;
            }
        }

        // Create a L2 pagetable capability node if it wasn't found
        if (node == NULL) {

            // Allocate the new tree node
            node = slab_alloc(&st->slabs);
            node->left = NULL;
            node->right = NULL;
            node->subtree = NULL;

            // Allocate a new slot for the mapping capability
            errval_t err_slot_alloc = st->slot_alloc->alloc(st->slot_alloc, &node->mapping_cap);
            if (err_is_fail(err_slot_alloc)) {
                slab_free(&st->slabs, node);
                return err_slot_alloc;
            }

            // Allocate a new L2 pagetable and get the capability
            errval_t err_l2_alloc = arml2_alloc(st, &node->cap);
            if (!err_is_ok(err_l2_alloc)) {
                slot_free(node->mapping_cap);
                slab_free(&st->slabs, node);
                return err_l2_alloc;
            }
            
            // Check for reentrant call of this function
            int skip_l2_creation = 0;
            if (prev && prev->offset > l1_offset && prev->left != NULL) {
                struct pt_cap_tree_node *same_node = prev->left;
                while (same_node != NULL) {
                    // Check if reentrant call created a node for this offset
                    if (l1_offset == same_node->offset) {
                        slab_free(&st->slabs, node);
                        node = same_node;
                        skip_l2_creation = 1;
                        break;
                    }
                    // Move prev to avoid overwriting an existing node
                    prev = same_node;
                    if (l1_offset < same_node->offset) {
                        same_node = same_node->left;
                    }
                    else if (l1_offset > same_node->offset) {
                        same_node = same_node->right;
                    }
                }
            }
            else if (prev && prev->offset < l1_offset && prev->right != NULL) {
                struct pt_cap_tree_node *same_node = prev->right;
                while (same_node != NULL) {
                    // Check if reentrant call created a node for this offset
                    if (l1_offset == same_node->offset) {
                        slab_free(&st->slabs, node);
                        node = same_node;
                        skip_l2_creation = 1;
                        break;
                    }
                    // Move prev to avoid overwriting an existing node
                    prev = same_node;
                    if (l1_offset < same_node->offset) {
                        same_node = same_node->left;
                    }
                    else if (l1_offset > same_node->offset) {
                        same_node = same_node->right;
                    }
                }
            }
            if (!skip_l2_creation) {
                
                // Map L2 pagetable to appropriate slot in L1 pagetable
                errval_t err_l2_map = vnode_map(st->l1_pagetable, node->cap, l1_offset, flags, 0, 1, node->mapping_cap);
                if (!err_is_ok(err_l2_map)) {
                    slot_free(node->cap);
                    slot_free(node->mapping_cap);
                    slab_free(&st->slabs, node);
                    return err_l2_map;
                }
                
                // Set the offset for the new node
                node->offset = l1_offset;

                // Store new node in the tree
                if (st->l2_tree_root == NULL) {
                    st->l2_tree_root = node;
                }
                else if (prev->offset > l1_offset) {
                    prev->left = node;
                }
                else {
                    prev->right = node;
                }
                
            }

        }

        // Calculate the number of pages that need to be allocated
        int num_pages = size / BASE_PAGE_SIZE;
        if (size % BASE_PAGE_SIZE) {
            num_pages++;
        }

        // Allocate a new node for the new mapping
        struct pt_cap_tree_node *map_node = slab_alloc(&st->slabs);
        map_node->left = NULL;
        map_node->right = NULL;
        map_node->subtree = NULL;

        // Allocate a new slot for the mapping capability
        errval_t err_slot_alloc = st->slot_alloc->alloc(st->slot_alloc, &map_node->mapping_cap);
        if (!err_is_ok(err_slot_alloc)) {
            slab_free(&st->slabs, map_node);
            return err_slot_alloc;
        }

        // Map the frame into the appropriate slot in the L2 pagetable
        errval_t err_frame_map = vnode_map(node->cap, frame, l2_offset, flags, addr - vaddr, num_pages, map_node->mapping_cap);
        if (!err_is_ok(err_frame_map)) {
            slot_free(map_node->mapping_cap);
            slab_free(&st->slabs, map_node);
            return err_frame_map;
        }

        // Store the frame capability and L2 page table offset
        map_node->cap = frame;
        map_node->offset = mapping_offset;

        // Store the new node in the mapping capability tree
        if (node->subtree == NULL) {
            node->subtree = map_node;
        } else {
            struct pt_cap_tree_node *prev_map = node->subtree;
            while (prev_map != NULL) {
                if (mapping_offset == prev_map->offset) {
                    debug_printf("Mapping capability already in mapping tree\n");
                }
                else if (mapping_offset < prev_map->offset) {
                    if (prev_map->left != NULL) {
                        prev_map = prev_map->left;
                    } else {
                        prev_map->left = map_node;
                        break;
                    }
                }
                else if (mapping_offset > prev_map->offset) {
                    if (prev_map->right != NULL) {
                        prev_map = prev_map->right;
                    } else {
                        prev_map->right = map_node;
                        break;
                    }
                }
            }
        }
        
        // Check that there are sufficient slabs left in the slab allocator
        size_t freecount = slab_freecount((struct slab_allocator *)&st->slabs);
        if (freecount <= 6 && !st->slabs_prevent_refill) {
#if PRINT_DEBUG
            debug_printf("Paging slabs allocator refilling...\n");
#endif
            st->slabs_prevent_refill = 1;
            slab_default_refill((struct slab_allocator *)&st->slabs);
            st->slabs_prevent_refill = 0;
        }

    }
    
    // Summary
#if PRINT_DEBUG
    debug_printf("Finished mapping!\n");
#endif
    
    return SYS_ERR_OK;
}

/**
 * \brief unmap a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 * NOTE: Implementing this function is optional.
 */
errval_t paging_unmap(struct paging_state *st, const void *region)
{
    errval_t err;
    
    // Free memory by moving node from vspace free list to vspace alloc list
    size_t ret_size;
    err = paging_free(st, region, &ret_size);
    if (err_is_fail(err)) {
        return err;
    }

    // Actually unmapping region in memory (possibly over multiple l2 pagetables)
    err = paging_unmap_fixed(st, (lvaddr_t) region, ret_size);
    
    return err;
}

errval_t paging_free(struct paging_state *st, const void *region, size_t *ret_size) {
    
    errval_t err;
    
    // Searching for node in alloc linked list
    struct vspace_node *ret_node;
    err = delete_vspace_alloc_node(st, (lvaddr_t) region, &ret_node);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Returning size of freed memory
    *ret_size = ret_node->size;
    
    // Insert ret_node in vspace free linked list (coalescing included)
    err = insert_vspace_free_node(st, ret_node);
    
    return err;
}

errval_t paging_unmap_fixed(struct paging_state *st, lvaddr_t vaddr, size_t bytes) {

#if PRINT_DEBUG
    debug_printf("Unmapping %d page(s) at 0x%x\n", bytes / BASE_PAGE_SIZE + (bytes % BASE_PAGE_SIZE ? 1 : 0), vaddr);
#endif
    
    for (uintptr_t end_addr, addr = vaddr; addr < vaddr + bytes; addr = end_addr) {
        
        // Find next boundary of L2 page table range
        end_addr = addr / (ARM_L2_MAX_ENTRIES * BASE_PAGE_SIZE);
        end_addr++;
        end_addr *= (ARM_L2_MAX_ENTRIES * BASE_PAGE_SIZE);
        
        // Calculate the offsets for the given virtual address
        uintptr_t l1_offset = ARM_L1_OFFSET(addr);
        //uintptr_t l2_offset = ARM_L2_OFFSET(addr);            // TODO: Use l2_offset instead of mapping_offset as key in subtee
        uintptr_t mapping_offset = addr / BASE_PAGE_SIZE;
        
        // Searching for l2 pagetable capability in the l2 tree with l1_offset as key
        struct pt_cap_tree_node *l2_node = st->l2_tree_root;
        while (l2_node != NULL) {
            
            if (l1_offset < l2_node->offset) {
                l2_node = l2_node->left;
            } else if (l1_offset > l2_node->offset) {
                l2_node = l2_node->right;
            } else {
                break;
            }
            
        }
        
        // Check if l2 tree node was found
        if (l2_node == NULL) {
            debug_printf("l2 node in l2 tree not found");
            return MM_ERR_NOT_FOUND;
        }
        
        // Searching for mapping capability in the subtree of l2 node with mapping_offset as key
        struct pt_cap_tree_node **node_indirect = &l2_node->subtree;
        while (*node_indirect != NULL) {
            
            if (mapping_offset > (*node_indirect)->offset) {
                node_indirect = &(*node_indirect)->right;
            } else if (mapping_offset < (*node_indirect)->offset) {
                node_indirect = &(*node_indirect)->left;
            } else {
                break;
            }
            
        }
        
        // Check if mapping tree node was found
        if (*node_indirect == NULL) {
            debug_printf("mapping node in subtree not found");
            return MM_ERR_NOT_FOUND;
        }
        
        struct pt_cap_tree_node *deletion_node = NULL;
        
        // Check children of deletion node
        if ((*node_indirect)->left != NULL && (*node_indirect)->right != NULL) {
            
            debug_printf("HAS LEFT AND RIGHT CHILD\n");
            
            // Finding successor to swap with deletion node
            struct pt_cap_tree_node **succ_indirect = &(*node_indirect)->right;
            while((*succ_indirect)->left != NULL) {
                succ_indirect = &(*succ_indirect)->left;
            }
            
            // Relink successor parent with successor child
            struct pt_cap_tree_node *succ = *succ_indirect;
            *succ_indirect = (*succ_indirect)->right;
            
            // Change children of actual successor to have children of deletion node
            succ->left = (*node_indirect)->left;
            succ->right = (*node_indirect)->right;
            
            deletion_node = *node_indirect;
            
            // Setting new child of parent node
            *node_indirect = succ;
            
        } else if ((*node_indirect)->left != NULL) {
            
            debug_printf("HAS LEFT CHILD\n");
            
            deletion_node = *node_indirect;
            
            // Setting new child of parent node
            *node_indirect = (*node_indirect)->left;
            
        } else if ((*node_indirect)->right != NULL) {
            
            debug_printf("HAS RIGHT CHILD\n");
            
            deletion_node = *node_indirect;
            
            // Setting new child of parent node
            *node_indirect = (*node_indirect)->right;
            
        } else {
            
            debug_printf("HAS NO CHILDREN\n");
            
            deletion_node = *node_indirect;
            
            // Setting new child of parent node
            *node_indirect = NULL;
            
        }
        
        // Freeing capabilites cap and mapping_cap of deletion_node and tree node slab
        debug_printf("Deleting capabilities and freeing slab/slots of deletion node\n");
        
        errval_t err;
        
        // Unmapping mapping_cap from l2 pagetable
        err = vnode_unmap(l2_node->cap, deletion_node->mapping_cap);
        if (err_is_fail(err)) {
            return err;
        }
        
        // Deleting deletion_node mapping capability
        err = cap_delete(deletion_node->mapping_cap);
        if (err_is_fail(err)) {
            return err;
        }
        
        // Freeing deletion_node mapping capability slot
        slot_free(deletion_node->mapping_cap);
        
        // Deleting deletion_node capability
        err = cap_delete(deletion_node->cap);
        if (err_is_fail(err)) {
            return err;
        }
        
        // Freeing deletion_node capability slot
        slot_free(deletion_node->cap);
        
        // Freeing tree deletion_node slab
        slab_free(&st->slabs, deletion_node);
        
        debug_printf("Deleted capabilities and freed slab/slots of deletion node\n");
        
    }
    
    debug_printf("Unmapped %d page(s) at 0x%x\n", bytes / BASE_PAGE_SIZE + (bytes % BASE_PAGE_SIZE ? 1 : 0), vaddr);

    return SYS_ERR_OK;

}

static errval_t delete_vspace_alloc_node(struct paging_state *st, lvaddr_t base, struct vspace_node **ret_node) {
    
    // Searching through alloc linked list
    struct vspace_node **node_indirect = &st->alloc_vspace_head;
    while (*node_indirect != NULL) {
        
        if ((*node_indirect)->base == base) {
            break;
        }
        
    }
    
    if (*node_indirect == NULL) {
        debug_printf("alloc node was not found");
        return LIB_ERR_VSPACE_VREGION_NOT_FOUND;
    }
    
    *ret_node = *node_indirect;
    
    // Cleaning up ret_node->next needed?
    (*ret_node)->next = NULL;
    
    // Remove node from linked list by changing next of previous node
    *node_indirect = (*node_indirect)->next;
    
    return SYS_ERR_OK;
    
}

static errval_t insert_vspace_free_node(struct paging_state *st, struct vspace_node *new_node) {
    
    // Check if list empty or node would have to get new head
    if (st->free_vspace_head == NULL || new_node->base + new_node->size < st->free_vspace_head->base) {
        
        // Add new_node to head of the linked list
        new_node->next = st->free_vspace_head;
        st->free_vspace_head = new_node;
        
        return SYS_ERR_OK;
        
    }
    else if (new_node->base + new_node->size == st->free_vspace_head->base) {
        
        // Coalescing with front of head
        st->free_vspace_head->base = new_node->base;
        st->free_vspace_head->size += new_node->size;
        
        // Free the memory/slab for new_node
        slab_free(&st->vspace_slabs, new_node);

        return SYS_ERR_OK;
        
    }
    
    // Iterating through loop adding new_node if necessary,
    // otherwise coalescing by changing base and size of exisiting node
    // and freeing slab of new_node
    struct vspace_node *node;
    for (node = st->free_vspace_head; node != NULL; node = node->next) {
        
        if (node->next != NULL &&
            node->base + node->size == new_node->base &&
            new_node->base + new_node->size == node->next->base) {
            
            // Coalescing with back of node and front of node->next
            node->size += new_node->size + node->next->size;
            // Remove the node->next from the linked list
            struct vspace_node *next_node = node->next;
            node->next = node->next->next;
            
            // Free the memory/slab for node->next
            slab_free(&st->vspace_slabs, next_node);
            
            // Free the memory/slab for new_node
            slab_free(&st->vspace_slabs, new_node);
            
            break;
            
        }
        else if (node->next != NULL &&
                 new_node->base + new_node->size == node->next->base) {
            
            // Coalescing with front of node->next
            node->next->base = new_node->base;
            node->next->size += new_node->size;
            
            // Free the memory/slab for new_node
            slab_free(&st->vspace_slabs, new_node);
            
            break;
        }
        else if (node->base + node->size == new_node->base) {
            
            // Coalescing with back of node
            node->size += new_node->size;
            
            // Free the memory/slab for new_node
            slab_free(&st->vspace_slabs, new_node);
            
            break;
        
        }
        else if (node->base + node->size < new_node->base){
            
            // Add new_node to the linked list between node and node->next (which is potentially null)
            new_node->next = node->next;
            node->next = new_node;
            
            break;
            
        }

    }
    
    return SYS_ERR_OK;
    
}


