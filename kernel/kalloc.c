// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  int free;
} kmem[NCPU];

static char *kmem_lock_names[] = {
    "kmem_0",
    "kmem_1",
    "kmem_2",
    "kmem_3",
    "kmem_4",
    "kmem_5",
    "kmem_6",
    "kmem_7",
};

void
kinit()
{
  initlock(&kmem[0].lock, kmem_lock_names[0]);
  kmem[0].free = 0;
  freerange(end, (void*)PHYSTOP);
}
struct run* memory_from_other_cpus(int cur_cpuid, int steal_num);
void
kinit_core(int cpu_id)
{
  printf("initialize core %d\n", cpu_id);
  initlock(&kmem[cpu_id].lock, kmem_lock_names[cpu_id]);
  acquire(&kmem[cpu_id].lock);
  kmem[cpu_id].free = 0;
  kmem[cpu_id].freelist = memory_from_other_cpus(cpu_id, 8182);// 32731/4 (freerange)
  release(&kmem[cpu_id].lock);
}

void
freerange(void *pa_start, void *pa_end)
{
  uint64 no_pages = ((uint64)pa_end-(uint64)pa_start)/(uint64)PGSIZE;
  printf("pa start = %p, pa end = %p, no of pages = %ld\n", pa_start, pa_end, no_pages);
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  kmem[cpu_id].free++;
  release(&kmem[cpu_id].lock);
  pop_off();
}

struct run*
memory_from_other_cpus(int cur_cpuid, int steal_num)
{
  struct run *head, *tail;
  int stolen;
#define PASS2
#ifdef PASS2
  // First pass: look for a CPU with enough pages.
  for(int pass = 0; pass < 2; pass++) {
#endif
    for(int i = 0; i < NCPU; i++) {
      if(i == cur_cpuid)
        continue;

      acquire(&kmem[i].lock);
      if(kmem[i].freelist == 0 ||
         (pass == 0 && kmem[i].free < steal_num)) {
        release(&kmem[i].lock);
        continue;
      }

      head = kmem[i].freelist;
      tail = head;
      stolen = 1;

      while(stolen < steal_num && tail->next) {
        tail = tail->next;
        stolen++;
      }

      kmem[i].freelist = tail->next;
      tail->next = 0;

      kmem[i].free -= stolen;
      kmem[cur_cpuid].free += stolen;

      release(&kmem[i].lock);
      return head;
    }
#ifdef PASS2
  }
#endif

  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  if(kmem[cpu_id].freelist == 0){
    kmem[cpu_id].freelist = memory_from_other_cpus(cpu_id, 200);
  }
  r = kmem[cpu_id].freelist;
  if(r){
    kmem[cpu_id].freelist = r->next;
    kmem[cpu_id].free--;
  }
  release(&kmem[cpu_id].lock);
  pop_off();
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
