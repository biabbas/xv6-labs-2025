//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

#ifdef DEBUG
#define dbg_print(a) printf a
#else
#define dbg_print(a) ((void)0)
#endif
// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

struct mmap_struct* freelist = 0;
struct page_cache* pages_freelist=0;
struct spinlock freelist_lock;
struct spinlock pagecache_lock;

void mmap_global_locks_init()
{
  initlock(&freelist_lock, "mmapstruct freelist lock");
  initlock(&pagecache_lock, "page cache mmap lock");
}

void free_small_block(struct mmap_struct* mapping)
{
  dbg_print(("small_block_allocator: freed: %p\n", mapping));
  acquire(&freelist_lock);
  if(freelist == 0){
    mapping->next = mapping;
    mapping->prev = mapping;
    freelist = mapping;
  }
  else{
    mapping->next = freelist;
    freelist->prev->next = mapping;
    mapping->prev = freelist->prev;
    freelist->prev = mapping;
  }
  release(&freelist_lock);
}

void* get_small_block()
{
  struct mmap_struct* new_mapping;
  if(freelist == 0){
    new_mapping = kalloc();
    if(new_mapping == 0)
      panic("mmap: kalloc out of memory");
    for(int i=1;i<PGSIZE/sizeof(struct mmap_struct); i++){
      free_small_block(&new_mapping[i]);
    }
  }
  else{
    acquire(&freelist_lock);
    if(freelist->next == freelist){
      if(freelist->prev != freelist)
        panic("mmap: getmapping doubly list not in assumed state");
      new_mapping = freelist;
      freelist = 0;
    }
    else{
      new_mapping = freelist->prev;
      new_mapping->prev->next = freelist;
      freelist->prev = new_mapping->prev;
      new_mapping->prev = new_mapping->next = 0;
    }
    release(&freelist_lock);
  }
  dbg_print(("small_block_allocator: Allocated: %p\n", new_mapping));
  return new_mapping;
}

struct page_cache* find_page(struct inode* ip, off_t fileoff){
  acquire(&pagecache_lock);
  struct page_cache* node = pages_freelist;
  while(node != 0){
    if(node->f_ip == ip && node->file_offset == fileoff){
      release(&pagecache_lock);
      return node;
    }
    if(node->next == pages_freelist)
      break;
    else
      node = node->next;
  }
  release(&pagecache_lock);
  return 0;
}

struct page_cache* get_page_cache(struct inode* ip, off_t fileoff){
  struct page_cache* p_cache;
  if((p_cache = find_page(ip, fileoff)) != 0)
    return p_cache;
  struct page_cache* new_cache = get_small_block();
  new_cache->f_ip = ip;
  new_cache->file_offset = fileoff;
  uint64 page = (uint64)kalloc();
  if(page == 0)
    panic("mmap page cache: kalloc out of memory");
  ilock(ip);
  int readsize = readi(ip, 0, page, fileoff, PGSIZE);
  iunlock(ip);
  memset((void*)page+readsize, 0, PGSIZE-readsize);
  new_cache->pa = page;
  new_cache->ref_count = 0;
  acquire(&pagecache_lock);
  if(pages_freelist == 0){
    new_cache->next = new_cache;
    new_cache->prev = new_cache;
    pages_freelist = new_cache;
  }
  else{
    new_cache->next = pages_freelist;
    pages_freelist->prev->next = new_cache;
    new_cache->prev = pages_freelist->prev;
    pages_freelist->prev = new_cache;
  }
  release(&pagecache_lock);
  return new_cache;
}

int write_page(struct page_cache* pcache){
  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  int n = PGSIZE;
  int n1;
  int i = 0;
  int r;
  while(i<n){
    n1 = n-i;
    if(n1 > max)
      n1 = max;
    begin_op();
    ilock(pcache->f_ip);
    r = writei(pcache->f_ip, 0, pcache->pa+i, pcache->file_offset+i, n1);
    iunlock(pcache->f_ip);
    end_op();
    if(r != n1)
      break;
    i+=r;
  }
  return i != n;
}

void remove_from_page_cache_list(struct page_cache* p_cache){
  acquire(&pagecache_lock);
  p_cache->prev->next = p_cache->next;
  p_cache->next->prev = p_cache->prev;
  if(p_cache->next == p_cache){
    pages_freelist = 0;
  }
  else if(pages_freelist == p_cache)
    pages_freelist = p_cache->next; 
  release(&pagecache_lock);
}

struct page_cache* find_pcache_pa(uint64 pa)
{
  struct page_cache* p_cache = pages_freelist;
  acquire(&pagecache_lock);
  while(p_cache != 0){
    if(p_cache->pa == pa){
      break;
    }
    if(p_cache->next == pages_freelist)
      break;
    else
      p_cache = p_cache->next;
  }
  release(&pagecache_lock);
  return p_cache->pa == pa? p_cache: 0;
}

void page_cache_free(uint64 pa, int write_page_to_file){
  struct page_cache* p_cache = find_pcache_pa(pa);
  if(p_cache == 0)
    panic("Page cache entry not found");
  if(write_page_to_file)
    if(write_page(p_cache) != 0)
      panic("writing page failed in mmap shared");
  p_cache->ref_count--;
  if(p_cache->ref_count > 0)
    return;
  remove_from_page_cache_list(p_cache);
  kfree((void*)p_cache->pa);
  free_small_block((void*)p_cache);
}

// For a va, return mmap struct if va is within the struct's range
// -1 if vma does not exist.
struct mmap_struct* find_mmap(uint64 va, struct mmap_struct* mmap_v){
  struct mmap_struct* node = mmap_v;
  while(node != 0){
    if(va - node->start_va < node->length)
      return node;
    if(node->next == mmap_v)
      break;
    else
      node = node->next;
  }
  return (void*)(uint64)-1;
}

struct mmap_struct* detect_overlap(uint64 va, int length, struct mmap_struct* mmap_v){
  struct mmap_struct* node = mmap_v;
  while(node != 0){
    if(va - node->start_va < node->length)
      return node;
    if(node->start_va - va < length)
      return node;
    if(node->next == mmap_v)
      break;
    else
      node = node->next;
  }
  return 0;
}

int store_mmap(struct inode* ip, uint64 va, off_t f_off, int length, void** mmapv_ptr, int flags, int perms)
{
  dbg_print(("store_mmap: entry: va: %p length %d, pid = %d\n", (void*)va, length, myproc()->pid));
  if(detect_overlap(va, length, *mmapv_ptr) != 0){
    printf("sys_mmap(): overlap detected\n");
    return -1;
  }
  struct mmap_struct* mmap_v = get_small_block();
  struct mmap_struct* list;
  mmap_v->start_va = va;
  mmap_v->length = length;
  mmap_v->f_ip = idup(ip);
  mmap_v->file_offset = f_off;
  mmap_v->flags = flags;
  mmap_v->prot = perms;
  if(*mmapv_ptr == 0)
  {
    mmap_v->next = mmap_v;
    mmap_v->prev = mmap_v;
    *mmapv_ptr = mmap_v;
  }
  else {
    list = *mmapv_ptr;
    list->prev->next = mmap_v;
    mmap_v->next =list;
    mmap_v->prev = list->prev;
    list->prev = mmap_v; 
  }
  return 0;
}

uint64 find_va(int length, uint64 program_size, struct mmap_struct* mmap_list)
{
  uint64 init = TRAPFRAME-PGSIZE;
  init-=length;
  while(init > program_size){
    struct mmap_struct* overlap = detect_overlap(init, length, mmap_list);
    if(overlap == 0){
      dbg_print(("Suggest mmap range: %p, length %d, pid = %d\n", (void*)init, length, myproc()->pid));
      return init;
    }
    else
      init = overlap->start_va-length;
  }
  return -1;
}

uint64
sys_mmap(void)
{
  uint64 addr;
  size_t length;
  off_t offset;
  int prot, flags;
  struct file* f;
  struct proc* p = myproc();
  argaddr(0, &addr);
  argaddr(1, &length);
  if(length <= 0)
    return -1;
  argint(2, &prot);
  argint(3, &flags);
  if(argfd(4, 0, &f) < 0){
    printf("sys_mmap(): file associated with fd not found, pid = %d\n", p->pid);
    return -1;
  }
  argaddr(5, (unsigned long*)&offset);
  if((addr | offset | length) & 0xfff){
    printf("sys_mmap(): page offset or addr not aligned, pid = %d\n", p->pid);
    return -1;
  }

  if((addr < p->sz) || (detect_overlap(addr, length, p->mmap_list) != 0))
    if((addr = find_va(length,p->sz, p->mmap_list)) == -1){
      printf("sys_mmap(): Failed to adjust virtual address, pid = %d\n", p->pid);
      return -1;
    }
  if(!f->writable)
    if((prot&PROT_WRITE) && (flags & MAP_SHARED)){
      printf("sys_mmap(): opening a readonly file with write permission not allowed, pid = %d\n", p->pid);
      return -1;
    }
  int perms = ((prot&PROT_READ)? PTE_R:0) | ((prot&PROT_EXEC)? PTE_X:0) | ((prot&PROT_WRITE)?PTE_W:0);
  
  if((uint64)store_mmap(f->ip, addr, offset, length, &p->mmap_list, flags, perms) == -1)
    return -1;

  dbg_print(("sys_mmap: Successfully mapped addr = %p, pid = %d\n",(void*)addr, p->pid));

  return addr;
}

struct mmap_struct* remove_from_list(struct mmap_struct* mentry, void** mmap_list){
  mentry->prev->next = mentry->next;
  mentry->next->prev = mentry->prev;
  if(mentry->next == mentry){
    *mmap_list = 0;
  }
  else if(*mmap_list == mentry)
    *mmap_list = mentry->next;
  mentry->next = mentry->prev = 0;
  return mentry;
}

void
unmap_vma_and_free_struct(struct mmap_struct* mentry, pagetable_t pagetable, int free)
{
  pte_t* pte;
  uint64 page;
  dbg_print(("unmapping_vma_and_free_struct: unmapping: %p, length = %ld, pid = %d\n", (void*)mentry->start_va, mentry->length, myproc()->pid));
  while(mentry->length > 0){
    pte = walk(pagetable, mentry->start_va, 0);
    if((*pte != 0) && (*pte & PTE_V)){
      page = PTE2PA(*pte);
      if((mentry->flags & MAP_PRIVATE) && (*pte & PTE_W)) // Directly free physically backed mmap private pages
        kfree((void*)page);
      else
        page_cache_free(page, *pte & PTE_D);
    }
    *pte=0;
    mentry->start_va+=PGSIZE;
    mentry->length-=PGSIZE;
  }
  if(free){
    begin_op();
    iput(mentry->f_ip);
    end_op();
    free_small_block(mentry);
  }
}

void
unmap_mmaplist(pagetable_t pagetable, void* mmap_list){
  struct mmap_struct* node = mmap_list;
  struct mmap_struct* next;
  while(node != 0){
    next = node->next;
    unmap_vma_and_free_struct(node, pagetable,1);
    if(next == mmap_list)
      break;
    node = next;
  }
}

void*
mmap_vma_clone(void* mmap_list){
  struct mmap_struct* new = 0;
  struct mmap_struct* node = mmap_list;
  while(node != 0){
    store_mmap(node->f_ip, node->start_va, node->file_offset,
    node->length, (void**)&new, node->flags, node->prot);
    if(node->next == mmap_list)
      break;
    node = node->next;
  }
  return new;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  size_t size;
  argaddr(0, &addr);
  argaddr(1, &size);
  if((size | addr ) & 0xfff)
    return -1;

  uint64 va;
  uint64 length;
  struct mmap_struct* mentry;
  struct proc* p = myproc();
  dbg_print(("sys_munmap: %p, to %p, pid =  %d\n", (void*)addr, (void*)(addr+size), p->pid));
  va = addr;
  length = size;

  while(length > 0){
    mentry = find_mmap(va, p->mmap_list);
    if((uint64)mentry == -1){
      printf("sys_munmap(): unmapping other vma not allowed, pid = %d\n", p->pid);
      return -1;
    }
    if(mentry->start_va == addr)
    {
      remove_from_list(mentry, &p->mmap_list);
      if(mentry->length > length){
        int ret = store_mmap(mentry->f_ip, mentry->start_va+length, mentry->file_offset+length, mentry->length-length, &p->mmap_list, mentry->flags, mentry->prot);
        if(ret)
        panic("sys_munmap: unexpected vma state");
        mentry->length = length;
      }
      length-=mentry->length;
      va+=mentry->length;
      unmap_vma_and_free_struct(mentry, p->pagetable, 1);
    }
    else
    { // Address to be unmapped starts within this mentry.
      struct mmap_struct entry = *mentry;
      mentry->length = va-mentry->start_va;
      entry.start_va = va;
      entry.file_offset+=mentry->length;
      entry.length-=mentry->length;
      while(entry.length > length){
        if(
        store_mmap(mentry->f_ip, entry.start_va+length, entry.file_offset+length, entry.length-length, &p->mmap_list, entry.flags, entry.prot))
        panic("sys_munmap: Unexpected vma list state");
        entry.length = length;
      }
      length-=entry.length;
      va+=entry.length;
      unmap_vma_and_free_struct(&entry, p->pagetable, 0);    
    }
  }
  if(length < 0)
    panic("sys_munmmap: unexpected state");
  return 0;
}

// Allocate mmapped page if valid.
// If mapped page is already 
uint64
mmap_fault(pagetable_t pagetable, uint64 va, int read, void* mmap_v)
{
  struct mmap_struct* mv =  find_mmap(va, mmap_v);
  pte_t* pte;
  void* page;
  struct page_cache* pcache;
  if((uint64)mv == -1)
    return 0;
  pte = walk(pagetable, va, 1);
  if(*pte & PTE_V){
    if(read || (*pte & PTE_W) || !(mv->flags & MAP_PRIVATE) || !(mv->prot & PTE_W))
      return 0;
    page = (void*)PTE2PA(*pte);
    pcache = find_pcache_pa((uint64)page);
    if(pcache == 0)
      return 0;
    if(pcache->ref_count == 1){
      remove_from_page_cache_list(pcache);
      *pte|=PTE_W;
      free_small_block((void*)pcache);
    }
    else{
      page = kalloc();
      if(page == 0)
        panic("mmap kalloc: out of pages");
      strncpy(page, (char*)pcache->pa, PGSIZE);
      pcache->ref_count--;
      *pte = PA2PTE(page) | PTE_U | mv->prot | PTE_V;
    }
    goto return_stmt;
  }
  struct inode* f_ip = mv->f_ip;
  off_t file_off = (va-mv->start_va)+mv->file_offset;
  if((mv->flags & MAP_SHARED) || read) {
  pcache = get_page_cache(mv->f_ip, file_off);
  page = (void*)pcache->pa;
  pcache->ref_count++;
  }
  else {
    page = kalloc();
    pcache = find_page(mv->f_ip, file_off);
    if(page == 0)
      panic("mmap kalloc: out of pages");
    if(pcache == 0){
      ilock(f_ip);
      int read_size = readi(f_ip, 0, (uint64)page, file_off, PGSIZE);
      iunlock(f_ip);
      memset(page+read_size, 0, PGSIZE-read_size);
    }
    else
      strncpy(page, (void*)pcache->pa, PGSIZE);
  }
  *pte = PA2PTE(page) | PTE_U | mv->prot | PTE_V;
  if((mv->flags & MAP_PRIVATE) && read)
    *pte&= ~(pte_t)PTE_W;
  return_stmt:
  dbg_print(("MMap page fault handled: provided va = %p, mv = %p, pid = %d\n", (void*)va, mv , myproc()->pid));
  dbg_print(("file offset = %p, addr = %p, type = %s\n", (void*)mv->file_offset, (void*)mv->start_va, mv->flags&MAP_SHARED?"Map shared": "map private"));
  dbg_print(("mapped page %p\n", page));
  return (uint64)page;
}