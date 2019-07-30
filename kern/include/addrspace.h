/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

/*
 * Address space structure and operations.
 */


#include <vm.h>
#include "opt-dumbvm.h"

struct vnode;


// -------
// Structs
// -------

// Frame table - one entry for each physical frame. Keeps track of how many processes reference the address.
// Does not need to keep paddr, as the paddr is used to index into the frame table.
struct frame {
        int                 ref_count;
};

// Two - stage page table
struct secondary_page_entry {
	vaddr_t	               vaddr;    //This is the page num (first 20 bytes of vaddr)
	uint32_t	       paddr;    //The "low" that can be passed to tlb. If not set yet, it is USERSPACETOP
        int            copy_on_write;    //Defaults to 0. Is only 1 if it references physical memory of another process.
	int                    flags;    //The flags for permissions on the page. If not set, will be -1
};

struct root_page_entry {
    uint32_t 	           vaddr_prefix;   //First 10 bytes of the page num
    struct secondary_page_entry *target;   //Malloc'd secondary page, or NULL if not allocated
};

// Linked list to keep track of exactly which pages have been allocated
// (so we don't need to search entire PT each time)
struct region {
        vaddr_t       vbase;
        int       old_flags;
        struct region *next;
};


// ---------
// Constants
// ---------

// Number of entries in first page table
#define NUM_ROOT_ENTRIES (1 << 10)
// Number of entries in second page table
#define NUM_SECONDARY_ENTRIES (1 << 10)

#define RWX (PF_R | PF_W | PF_X)

/*
 * Address space - data structure associated with the virtual memory
 * space of a process.
 *
 * You write this.
 */

struct addrspace {
#if OPT_DUMBVM
        vaddr_t as_vbase1;
        paddr_t as_pbase1;
        size_t as_npages1;
        vaddr_t as_vbase2;
        paddr_t as_pbase2;
        size_t as_npages2;
        paddr_t as_stackpbase;
#else
        struct region *as_region;
        struct root_page_entry page_table[NUM_ROOT_ENTRIES];
        vaddr_t as_heap_start;	
	vaddr_t as_heap_end;	
#endif
};

// ----------
// Prototypes
// ----------
// (defined in vm.c)

// Ensures that vaddr is present in as's page table, and sets the flags as given.
// Will allocate a secondary page table if required.
void add_single_vaddr_page(struct addrspace *as, vaddr_t vaddr, int flags);

// Gets a page from as's page table. Returns the page table entry if found,
// returns NULL if secondary page table does not exist.
struct secondary_page_entry * get_page(struct addrspace *as, vaddr_t vaddr);

// Ensures that the page given has some physical memory.
// Does nothing if there is already physical memory,
//  allocates a single page if there is no physical memory.
void ensure_paddr(struct secondary_page_entry * page);

// Decrements the specific paddr in the frame table
// If ref_count drops to 0, will free the paddr
void decrement_ref_count(paddr_t paddr);

// Increments the ref count of paddr in frame table
// Use when copying, not allocating new memory
// If you need new physical memory, call ensure_paddr
void increment_ref_count(paddr_t paddr);

/*
 * Functions in addrspace.c:
 *
 *    as_create - create a new empty address space. You need to make
 *                sure this gets called in all the right places. You
 *                may find you want to change the argument list. May
 *                return NULL on out-of-memory error.
 *
 *    as_copy   - create a new address space that is an exact copy of
 *                an old one. Probably calls as_create to get a new
 *                empty address space and fill it in, but that's up to
 *                you.
 *
 *    as_activate - make curproc's address space the one currently
 *                "seen" by the processor.
 *
 *    as_deactivate - unload curproc's address space so it isn't
 *                currently "seen" by the processor. This is used to
 *                avoid potentially "seeing" it while it's being
 *                destroyed.
 *
 *    as_destroy - dispose of an address space. You may need to change
 *                the way this works if implementing user-level threads.
 *
 *    as_define_region - set up a region of memory within the address
 *                space.
 *
 *    as_prepare_load - this is called before actually loading from an
 *                executable into the address space.
 *
 *    as_complete_load - this is called when loading from an executable
 *                is complete.
 *
 *    as_define_stack - set up the stack region in the address space.
 *                (Normally called *after* as_complete_load().) Hands
 *                back the initial stack pointer for the new process.
 *
 * Note that when using dumbvm, addrspace.c is not used and these
 * functions are found in dumbvm.c.
 */

struct addrspace *as_create(void);
int               as_copy(struct addrspace *src, struct addrspace **ret);
void              as_activate(void);
void              as_deactivate(void);
void              as_destroy(struct addrspace *);

int               as_define_region(struct addrspace *as,
                                   vaddr_t vaddr, size_t sz,
                                   int readable,
                                   int writeable,
                                   int executable);
int               as_prepare_load(struct addrspace *as);
int               as_complete_load(struct addrspace *as);
int               as_define_stack(struct addrspace *as, vaddr_t *initstackptr);



// Identical to as_define_region, but doesn't modify heap.
int as_define_region_noheap(struct addrspace *as, vaddr_t vaddr, size_t memsize,
					 int readable, int writeable, int executable);
// Removes a region (as may happen when sbrk is called with a negative)
void as_remove_region(struct addrspace *as, vaddr_t vaddr, size_t memsize);


/*
 * Functions in loadelf.c
 *    load_elf - load an ELF user program executable into the current
 *               address space. Returns the entry point (initial PC)
 *               in the space pointed to by ENTRYPOINT.
 */

int load_elf(struct vnode *v, vaddr_t *entrypoint);


#endif /* _ADDRSPACE_H_ */
