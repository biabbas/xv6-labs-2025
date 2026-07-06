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
#define min(a, b) ((a) < (b) ? (a) : (b))

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

extern int ncpu_active;

// #define CPU_FREELIST_CACHE_LIMIT 8192
#ifdef CPU_FREELIST_CACHE_LIMIT
// This variable is used to check if required cpu kmem freelist are initialized.
static int kmem_list_nums = 0;
#endif

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
  kmem[0].freelist = 0;
  freerange(end, (void*)PHYSTOP);
#ifdef CPU_FREELIST_CACHE_LIMIT
  __sync_add_and_fetch(&kmem_list_nums, 1);
#endif
}

struct run* memory_from_other_cpus(int cur_cpuid, int steal_num, int* stolen_count);

void
kinit_core(int cpu_id)
{
  initlock(&kmem[cpu_id].lock, kmem_lock_names[cpu_id]);
  int stolen_count;
  struct run* new_freelist= memory_from_other_cpus(cpu_id, 8182, &stolen_count);// 32731/4 (freerange)
  printf("initialize core %d with %d pages freelist\n", cpu_id, stolen_count);
  acquire(&kmem[cpu_id].lock);
  kmem[cpu_id].free = stolen_count;
  kmem[cpu_id].freelist = new_freelist;
  release(&kmem[cpu_id].lock);
#ifdef CPU_FREELIST_CACHE_LIMIT
  kmem_list_nums++;
#endif
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
#ifdef CPU_FREELIST_CACHE_LIMIT
  int loss = 1;
  if((kmem_list_nums == 4) && (kmem[cpu_id].free > CPU_FREELIST_CACHE_LIMIT)){ // Donate to other cpu's
    release(&kmem[cpu_id].lock);
    for(int i = 0; i < ncpu_active; i++) {
      int check_cpu = (cpu_id+1+i)%ncpu_active;
      acquire(&kmem[check_cpu].lock);
      if((kmem[check_cpu].free > CPU_FREELIST_CACHE_LIMIT) && (check_cpu != cpu_id)) { // If we roundup to original cpu id, add new list to this only.
        release(&kmem[check_cpu].lock);
        continue;
      }
      r->next = kmem[check_cpu].freelist;
      kmem[check_cpu].freelist = r;
      kmem[check_cpu].free++;
      loss = 0;
      release(&kmem[check_cpu].lock);
      break;
    }
  }
  else{
    r->next = kmem[cpu_id].freelist;
    kmem[cpu_id].freelist = r;
    kmem[cpu_id].free++;
    loss = 0;
    release(&kmem[cpu_id].lock);
  }
  if(loss)
    panic("kfree: lost free memory");
#else
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  kmem[cpu_id].free++;
  release(&kmem[cpu_id].lock);
#endif
  pop_off();
}

struct run*
memory_from_other_cpus(int cur_cpuid, int steal_num, int* stolen_count)
{
  struct run *head, *tail;
  int stolen;
#define PASS2
#ifdef PASS2
  // First pass: look for a CPU with enough pages.
  for(int pass = 0; pass < 2; pass++) {
#endif
    for(int i = 0; i < ncpu_active; i++) {
      if(i == cur_cpuid)
        continue;

      acquire(&kmem[i].lock);
      if(kmem[i].freelist == 0
#ifdef PASS2
        || (pass == 0 && kmem[i].free < steal_num)
#endif
      ) {
        release(&kmem[i].lock);
        continue;
      }
      steal_num = min(steal_num, kmem[i].free/2);

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
      release(&kmem[i].lock);
      *stolen_count = stolen;
      return head;
    }
#ifdef PASS2
  }
#endif
  *stolen_count = 0;
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
#define CPU_PAGE_STEALING_SIZE 256
#if CPU_PAGE_STEALING_SIZE > 1
  if(kmem[cpu_id].freelist == 0){
    release(&kmem[cpu_id].lock);
    int stolen_count;
    struct run* new_freelist = memory_from_other_cpus(cpu_id, CPU_PAGE_STEALING_SIZE, &stolen_count);
    acquire(&kmem[cpu_id].lock);
    if(kmem[cpu_id].freelist != 0)
      panic("Kalloc: Unexpected kmem updated even after pushoff\n");
    kmem[cpu_id].freelist = new_freelist;
    kmem[cpu_id].free += stolen_count;
  }
  r = kmem[cpu_id].freelist;
  if(r){
    kmem[cpu_id].freelist = r->next;
    kmem[cpu_id].free--;
  }
  release(&kmem[cpu_id].lock);
#else
  r = kmem[cpu_id].freelist;
  if(r){
    kmem[cpu_id].freelist = r->next;
    kmem[cpu_id].free--;
  }
  release(&kmem[cpu_id].lock); // Important to release lock before stealing to avoid race conditions.
  if(r == 0){
    for(int j = 1; j < ncpu_active; j++) {
      int i = (cpu_id+j)%ncpu_active;

      acquire(&kmem[i].lock);
      if(kmem[i].freelist) {
        r = kmem[i].freelist;
        kmem[i].freelist = r->next;
        kmem[i].free--;
        release(&kmem[i].lock);
        break;
      }
      release(&kmem[i].lock);
    }
  }
#endif
  pop_off();
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
