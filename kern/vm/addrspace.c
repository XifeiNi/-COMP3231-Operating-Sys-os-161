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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <elf.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL)
	{
		return NULL;
	}

	as->as_region = NULL;
	as->as_heap_start = 0;
	as->as_heap_end = 0;

	// Initialise base page table
	int i;
	for (i=0; i < NUM_ROOT_ENTRIES; i++){
		as->page_table[i].vaddr_prefix = i;
		as->page_table[i].target = NULL;
	}

	return as;
}

int as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas == NULL)
	{
		return ENOMEM;
	}

	newas->as_heap_start = old->as_heap_start;
	newas->as_heap_end = old->as_heap_end;

	// Copy data to new address space
	struct region *cur = old->as_region;
	while (cur != NULL)
	{
		//Copy vaddr's by simply calling add_single_vaddr_page on new as
		struct secondary_page_entry *old_page = get_page(old, cur->vbase);
		add_single_vaddr_page(newas, cur->vbase, old_page->flags);

		// If physical memory exists, do not move it but instead set copy_on_write for all
		struct secondary_page_entry *new_page = get_page(newas, cur->vbase);
		if (old_page->paddr != USERSPACETOP){
			new_page->paddr = old_page->paddr;

			// Note that we don't just do this for pages with write permission
			// This is because as_prepare_load can change permissions.

			new_page->copy_on_write = 1;
			new_page->paddr = new_page->paddr & (~TLBLO_DIRTY); // Remove dirty from paddr, ie make readonly
			old_page->copy_on_write = 1;
			old_page->paddr = old_page->paddr & (~TLBLO_DIRTY); // Remove dirty from paddr, ie make readonly
			increment_ref_count(new_page->paddr);

			// We need to remove the old TLB entry if it exists
			int index = tlb_probe(new_page->vaddr<<12, 0);
			if (index >= 0){
				int spl = splhigh();
				tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
				splx(spl);
			}
		}
		cur = cur->next;
	}

	*ret = newas;
	return 0;
}

void as_destroy(struct addrspace *as)
{
	// Ensure the TLB is clean
	as_deactivate();

	struct region *region_node = as->as_region;
	struct region *temp_node;
	while (region_node != NULL)
	{
		// Delete any USEG memory
		struct secondary_page_entry *page = get_page(as, region_node->vbase);
		if (page->paddr != USERSPACETOP) {
			decrement_ref_count(page->paddr & TLBLO_PPAGE);
		}

		// Delete from region linked list
		temp_node = region_node->next;
		kfree(region_node);
		region_node = temp_node;
	}

	// Free secondary tables (if they're allocated)
	int i;
	for (i=0; i < NUM_ROOT_ENTRIES; i++){
		if (as->page_table[i].target != NULL) {
			kfree(as->page_table[i].target);
		}
	}
}

void as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL)
	{
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	int spl = splhigh();

	int i;
	for (i = 0; i < NUM_TLB; i++)
	{
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void as_deactivate(void)
{
	struct addrspace *as = proc_getas();
	if (as == NULL)
	{
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	int spl = splhigh();

	int i;
	for (i = 0; i < NUM_TLB; i++)
	{
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
					 int readable, int writeable, int executable)
{
	// Align the region's base
	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	// Align the region's size.
	memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

	int flags = readable | writeable | executable;

	// Update the heap on the writeable region
	// (ie data/bss segment, which I will assume is the only writable region as per loadelf.c)
	if (writeable) {
		as->as_heap_start = (vaddr & PAGE_FRAME) + memsize;
		as->as_heap_end = as->as_heap_start;
	}
	

	// Split the region up into pages, and allocate seperately for each
	vaddr_t i;
	for (i = 0; i < memsize; i += PAGE_SIZE) {
		// Add to PT
		add_single_vaddr_page(as, vaddr + i, flags);

		// Add to linked list, stack-style
		struct region* tmp = kmalloc(sizeof(struct region));
		if (tmp == NULL){
			return EFAULT;
		}
		tmp->vbase = vaddr+i;
		tmp->old_flags = flags;
		tmp->next = as->as_region;
		as->as_region = tmp;
	}

	return 0; 
}

// Identical to as_define_region, but doesn't modify heap.
int as_define_region_noheap(struct addrspace *as, vaddr_t vaddr, size_t memsize,
					 int readable, int writeable, int executable)
{
	// Align the region's base
	memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	// Align the region's size.
	memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

	int flags = readable | writeable | executable;


	// Split the region up into pages, and allocate seperately for each
	vaddr_t i;
	for (i = 0; i < memsize; i += PAGE_SIZE) {
		// Add to PT
		add_single_vaddr_page(as, vaddr + i, flags);

		// Add to linked list, stack-style
		struct region* tmp = kmalloc(sizeof(struct region));
		if (tmp == NULL){
			return EFAULT;
		}
		tmp->vbase = vaddr+i;
		tmp->old_flags = flags;
		tmp->next = as->as_region;
		as->as_region = tmp;
	}

	return 0; 
}

// Removes a region (as may happen when sbrk is called with a negative)
void as_remove_region(struct addrspace *as, vaddr_t vaddr, size_t memsize)
{
	struct region* prev = NULL;
	struct region* cur = as->as_region;

	while (cur != NULL){
		if (cur->vbase >= vaddr && cur->vbase < vaddr + memsize) {
			// Update pointers
			if (prev == NULL) {
				as->as_region = cur->next;
			} else {
				prev->next = cur->next;
			}

			// Actually delete (freeing any physical memory)
			struct region* tmp = cur->next;

			struct secondary_page_entry *page = get_page(as, cur->vbase);
			if (page->paddr != USERSPACETOP) {
				decrement_ref_count(page->paddr & TLBLO_PPAGE);
			}

			kfree(cur);

			cur = tmp;
		} else {
			prev = cur;
			cur = cur->next;
		}
	}
}

int as_prepare_load(struct addrspace *as)
{
	struct region *regions = as->as_region;

	while (regions) {
		struct secondary_page_entry *page = get_page(as, regions->vbase);

		regions->old_flags = page->flags; // Save old flags
		page->flags = page->flags | PF_W; // Ensure able to be written

		// Update paddr DIRTY flag if required
    	if ((page->paddr != USERSPACETOP) && ((page->paddr & TLBLO_DIRTY) == 0)) {
			// We need to remove the old TLB entry if it exists
			int index = tlb_probe(page->vaddr<<12, 0);
			if (index >= 0){
				int spl = splhigh();
				tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
				splx(spl);
			}

			page->paddr = page->paddr | TLBLO_DIRTY;
		}

		regions = regions->next;
	}
	return 0;
}

int as_complete_load(struct addrspace *as)
{
	struct region *regions = as->as_region;

	while (regions) {
		struct secondary_page_entry *page = get_page(as, regions->vbase);
		
		page->flags = regions->old_flags; // Restore previous permissions

		// Update paddr DIRTY flag if required
    	if ((page->paddr != USERSPACETOP) && ((page->flags & PF_W) == 0) && ((page->paddr & TLBLO_DIRTY) == 1)) {
			// We need to remove the old TLB entry if it exists
			int index = tlb_probe(page->vaddr<<12, 0);
			if (index >= 0){
				int spl = splhigh();
				tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
				splx(spl);
			}

			page->paddr = page->paddr & (!TLBLO_DIRTY);
		}

		regions = regions->next;
	}
	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	int err;
	if ((err = as_define_region_noheap(as, USERSTACK-USERSTACK_SIZE, USERSTACK_SIZE, 4, 2, 0)) != 0) {
		return err;
	}

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK-1;

	return 0;
}
