#include <types.h>  /* file for different types of OS161 */
#include <kern/sbrk.h> /* header file of this file */
#include <current.h>  /* for curthread */
#include <addrspace.h> /* heap start, end , etc */
#include <kern/errno.h> /* for different error constants */
#include <proc.h>
#include <elf.h>
#include <lib.h>

/* sys_sbrk function */
int sys_sbrk(int amount, int * retval) {

	// Round amount to multiple of 4
	if (amount % 4) {
		amount += (4 - amount % 4);
	}
    
 	// Get the end and start of the heap as well as the base of the stack
	vaddr_t heapEnd, heapStart;
   struct addrspace *as = proc_getas();

 	heapEnd = as->as_heap_end;
	heapStart = as->as_heap_start;

 	// parameter checking to see if heap end + amount < heap start
	if ((heapEnd + amount) < heapStart) {
		return EINVAL;	
	} else if (heapEnd + amount > USERSPACETOP) { // parameter checking to see if heap has not escaped user region
		return EINVAL;
	}

    if (amount > 536870912 || amount < -536870912) return ENOMEM;
 	// We are now clear to go ahead with the system call. But, before that return the old heap end through retval
	*retval = heapEnd;
	heapEnd += amount;

	// Please note through the following code that end is the bit after the end of the actual allocated region.

	if (amount > 0) {
		// May need more pages

		// This will indicate that new pages need to be allocated
		// Otherwise, it will already be covered in the page table.
		if ((((*retval) - 1) & PAGE_FRAME) != ((heapEnd - 1) & PAGE_FRAME)) {
			// We start allocation at the next page frame
			uint32_t newBase = (((*retval) - 1) & PAGE_FRAME) + PAGE_SIZE;
			as_define_region_noheap(as, newBase, heapEnd - newBase, PF_R, PF_W, PF_X);
		}
	} else if (amount < 0) {
		// Only delete pages larger than the current end
		uint32_t base = ((heapEnd - 1) & PAGE_FRAME) + PAGE_SIZE;
		// Here, memsize may be negative, but as_remove_region can deal with that (it will do nothing)
		as_remove_region(as, base, (*retval) - base);
	}

   as->as_heap_end = heapEnd;

 	// 0 indicates success
    return 0;


 }