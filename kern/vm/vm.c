#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spl.h>
#include <elf.h>
#include <current.h>
#include <proc.h>
#include <addrspace.h>

// Frame table
struct frame frame_table[NUM_ROOT_ENTRIES * NUM_SECONDARY_ENTRIES];

// Frame table functions

void decrement_ref_count(paddr_t paddr) {
    // Disable interrupts when changing frame table
    int spl = splhigh();
    paddr = paddr >> 12;

    (frame_table[paddr].ref_count)--;
    if (frame_table[paddr].ref_count == 0) {
        free_kpages(PADDR_TO_KVADDR(paddr << 12));
    }
    splx(spl);
}

void increment_ref_count(paddr_t paddr) {
    // Disable interrupts when changing frame table
    int spl = splhigh();
    paddr = paddr >> 12;

    KASSERT(frame_table[paddr].ref_count > 0); // If you need new pages, use ensure_paddr

    (frame_table[paddr].ref_count)++;
    splx(spl);
}

/* Place your page table functions here */

// Allocates a secondary page table at specified index of the root page table.
void create_secondary_table(struct addrspace *as, int index);


void add_single_vaddr_page(struct addrspace *as, vaddr_t address, int flags) {
    // Number of the page we are looking for
    uint32_t page_num = ((uint32_t) address) / PAGE_SIZE;

    // Get indices
    uint32_t prefix = page_num >> 10;
    uint32_t secondary_index = page_num ^ (prefix << 10);

    // Only fails if page table corrupt or hasn't been initialised
    KASSERT(as->page_table[prefix].vaddr_prefix == prefix);

    if (as->page_table[prefix].target == NULL){
        create_secondary_table(as, prefix);
    }

    struct secondary_page_entry *secondary_page = &(as->page_table[prefix].target[secondary_index]);
    KASSERT(secondary_page->vaddr == page_num);

    secondary_page->flags = flags;
}

struct secondary_page_entry * get_page(struct addrspace *as, vaddr_t address) {
    // Number of the page we are looking for
    uint32_t page_num = ((uint32_t) address) / PAGE_SIZE;

    // Get indices
    uint32_t prefix = page_num >> 10;
    uint32_t secondary_index = page_num ^ (prefix << 10);

    // If prefix is for an address which is too big
    if (prefix >= NUM_ROOT_ENTRIES) {
        return NULL;
    }

    // Only fails if page table corrupt or hasn't been initialised
    KASSERT(as->page_table[prefix].vaddr_prefix == prefix);

    //This function should not be called without calling add_single_vaddr_page first
    if (as->page_table[prefix].target == NULL) {
        return NULL;
    }

    return &(as->page_table[prefix].target[secondary_index]);
}

void ensure_paddr(struct secondary_page_entry * page){
    // We only need to add a paddr if it does not exist.
    if (page->paddr == USERSPACETOP){
        // Get a single page
        paddr_t new_page = KVADDR_TO_PADDR(alloc_kpages(1));

        if (PADDR_TO_KVADDR(new_page) == (int) NULL){
            panic("vm_fault: Unable to allocate new frame\n");
        }

        // Update frame table (without interrupts)
        int spl = splhigh();
        frame_table[new_page >> 12].ref_count = 1;
        splx(spl);

        // Zero-fill
        bzero((void *)PADDR_TO_KVADDR(new_page), PAGE_SIZE);

        // Set paddr up in the same form that gets passed to the tlb.
        uint32_t low = (TLBLO_PPAGE & new_page);

        int flags = page->flags;

        int writeable = flags & PF_W;

        if (writeable) {
            low = low | TLBLO_DIRTY;
        }

        low = low | TLBLO_VALID;

        page->paddr = low;
        KASSERT(page->paddr != USERSPACETOP);
    }

    KASSERT(page->paddr != USERSPACETOP);
}

// Creates and initialises a secondary page table, at the index specified
void create_secondary_table(struct addrspace *as, int prefix){

    // This function shouldn't be called on an existing secondary table
    KASSERT(as->page_table[prefix].target == NULL);

    as->page_table[prefix].target = kmalloc(NUM_SECONDARY_ENTRIES * sizeof(struct secondary_page_entry));
    if (as->page_table[prefix].target == NULL) {
        panic("vm_fault: could not allocate a secondary page table!\n");
    }

    int i;
    for (i=0; i < NUM_SECONDARY_ENTRIES; i++){
        as->page_table[prefix].target[i].vaddr = (prefix << 10) | i;
        as->page_table[prefix].target[i].paddr = USERSPACETOP;
        as->page_table[prefix].target[i].copy_on_write = 0;
        as->page_table[prefix].target[i].flags = -1;
    }
}

// ------------
// VM functions
// ------------


void vm_bootstrap(void)
{
    /* Initialise VM sub-system.  You probably want to initialise your 
       frame table here as well.
    */

   int i;
   for (i = 0; i < NUM_ROOT_ENTRIES * NUM_SECONDARY_ENTRIES; i++) {
       frame_table[i].ref_count = 0;
   }
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	struct addrspace* as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

    struct secondary_page_entry *page = get_page(as, faultaddress);

    switch (faulttype) {
	    case VM_FAULT_READONLY:
        if (page->copy_on_write) break;
		return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

    if (page == NULL || page->flags == -1) {
        // Indicates the address is invalid
        // (in that it was not allocated in the current process' address space.)
        return EFAULT;
    }

    if (page->copy_on_write && (faulttype == VM_FAULT_READONLY || faulttype == VM_FAULT_WRITE)) {
        // Disable interrupts when changing page table
        int spl = splhigh();

        paddr_t paddr = page->paddr;
        KASSERT(paddr != USERSPACETOP);

        if (frame_table[paddr >> 12].ref_count == 1) {
            // No other processes reference this paddr any more, no need to allocate
            page->copy_on_write = 0;
            // Make dirty again
            if (page->flags & PF_W) {
                page->paddr = page->paddr | TLBLO_DIRTY;
            }
        } else {
            page->paddr = USERSPACETOP;
            ensure_paddr(page);
            memmove((void*)PADDR_TO_KVADDR(page->paddr & TLBLO_PPAGE),
                    (const void*)PADDR_TO_KVADDR(paddr & TLBLO_PPAGE),
                    PAGE_SIZE);
            decrement_ref_count(paddr);

            page->copy_on_write = 0;
        }

        // We need to remove the old TLB entry if it exists
        int index = tlb_probe(page->vaddr<<12, 0);
        if (index >= 0){
            tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
        }

        splx(spl);
    } else {
        // Make sure there is physical memory here to write to
        ensure_paddr(page);
    }
    
    
    uint32_t high = faultaddress & TLBHI_VPAGE;
    int spl = splhigh();
    tlb_random(high, page->paddr);
    splx(spl);

    return 0;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

