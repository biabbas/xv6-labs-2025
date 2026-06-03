// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define NPAGE ((PHYSTOP - KERNBASE) / PGSIZE)
int refcount[NPAGE];

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    refcount[PA2IDX(p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// if only one process references that page. Otherwise
// reduce the reference counter for that page.
// Normally, The pa would have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int *rc;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  rc = &refcount[PA2IDX(pa)];
  acquire(&kmem.lock);

  if(*rc < 1){
    printf("pa = %p, refcount = %d\n", pa, *rc);
    panic("kfree: page should not be freed");
  }

  if(--(*rc) > 0){
    release(&kmem.lock);
    return;
  }
  // Filled with junk to catch dangling refs.
  // Currently this being inside a critical section affects the performance.
  // This memset is useful for debugging, hence it can be removed if the focus
  // is on performance.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;

  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    if((refcount[PA2IDX(r)]++) != 0)
      panic("kalloc: page with reference in freelist");
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// Resolve a copy-on-write page.
//
// Returns pa if it is not shared. Otherwise decrements pa's
// reference count and allocates a new page that the caller
// can copy into and map writable.
// Returns 0 on allocatin failure.
void*
cow_page_alloc(uint64 pa)
{
  int* rc;
  void* mem;
  rc = &refcount[PA2IDX(pa)];
  acquire(&kmem.lock);
  if(*rc == 1){
    release(&kmem.lock);
    return (void*)pa;
  }
  if(*rc < 1)
    panic("Ref alloc: refcount lower than expected");
  release(&kmem.lock);
  if ((mem = kalloc()) == 0)
    return 0;
  acquire(&kmem.lock);
  (*rc)--;
  release(&kmem.lock);
  memmove(mem, (void*)pa, PGSIZE);
  return mem;
}

// Increases reference counter for a given page
void
page_inc_ref(uint64 pa)
{
  acquire(&kmem.lock);
  refcount[PA2IDX(pa)]++;
  release(&kmem.lock);
}