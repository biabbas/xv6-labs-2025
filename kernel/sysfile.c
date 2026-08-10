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

uint64 find_va(int length, pagetable_t p_tbl, uint64 pmapped)
{
  uint64 init = TRAPFRAME;
  int found_length = 0;
  while(found_length < length)
  {
    init-=PGSIZE;
    printf("check va %p\n", (void*)init);
    if(ismapped(p_tbl, init)) {
      found_length = 0;
    }
    else{
      found_length+=PGSIZE;
    }
    if(init < pmapped)
      return -1;
  }
  return init;
}

struct mmap_struct mmap_v[MMAP_LIMIT];

struct mmap_struct* find_mmap(uint64 va){
  for(int i=0;i<MMAP_LIMIT;i++){
    if(mmap_v[i].va == va)
      return &mmap_v[i];
  }
  return (void*)(uint64)-1;
}
uint64 store_mmap(struct file* f, uint64 va, off_t f_off)
{
  for(int i=0;i<MMAP_LIMIT;i++){
    if(mmap_v[i].va == 0) {
      mmap_v[i].f = filedup(f);
      mmap_v[i].va = va;
      mmap_v[i].file_offset = f_off;
      // mmap_v[i].prot = prot;
      return i<<12;
    }
  }
  return -1;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses to pa
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() or store_mmap couldn't
// allocate required memory
static int
mappages_fixed_pa(pagetable_t pagetable, uint64 va, uint64 size, int perm, struct file* f, off_t offset)
{
  uint64 a, last;
  pte_t *pte;
  uint64 pa;
  off_t file_off = offset;
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pa = store_mmap(f, a, file_off))==-1)
      return -1;
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V | PTE_M;
    if(a == last)
      break;
    a += PGSIZE;
    file_off += PGSIZE;
  }
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 addr;
  size_t length;
  off_t offset;
  int prot, flags;
  struct file* f;
  argaddr(0, &addr);
  argaddr(1, &length);
  if(length <= 0)
    return -1;
  argint(2, &prot);
  argint(3, &flags);
  if(argfd(4, 0, &f) < 0){
    printf("mmap: file associated with fd not found\n");
    return -1;
  }
  argaddr(5, (unsigned long*)&offset);
  if((addr | offset | length) & 0xfff){
    printf("mmap: page offset or addr not aligned\n");
    return -1;
  }
  struct proc* p = myproc();
  if(addr == 0)
    if((addr = find_va(length, p->pagetable, p->sz)) == -1){
      printf("mmap: Failed to adjust virtual address\n");
      return -1;
    }
  if(!f->writable)
    if((prot&PROT_WRITE) && (flags & MAP_SHARED)){
      printf("mmap: opening a readonly file with write permission not allowed\n");
      return -1;
    }
  int perms = ((prot&PROT_READ)? PTE_R:0) | ((prot&PROT_EXEC)? PTE_X:0) | ((prot&PROT_WRITE)?PTE_W:0);

  if(mappages_fixed_pa(p->pagetable, addr, length, perms, f, offset) != 0){
    uvmunmap(p->pagetable, addr, length/PGSIZE, 0); // unmap any allocated pages
    printf("mmap: Failed to map pages\n");
    return -1;
  }
  printf("mmap successful addr = %p\n",(void*)addr);

  return addr;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  size_t length;
  argaddr(0, &addr);
  argaddr(1, &length);
  if((length | addr ) & 0xfff)
    return -1;
  
  uint64 va;
  pte_t* pte;
  struct mmap_struct* mentry;

  uint64 page;
  va = addr;
  for(int n=length/PGSIZE;n>0; n--) {
    pte = walk(myproc()->pagetable, va, 0);
    if((pte == 0) || !(*pte & PTE_M)){
      printf("munmap: trying to unmap other memory regions\n");
      return -1;
    }
    mentry = find_mmap(va);
    if((uint64)mentry == -1){
      printf("munmap: did not find mmap entry for some reason\n");
      return -1;
    }
    page = PTE2PA(*pte);

    printf("unmapped %p, %p\n", (void*)va, (void*)page);
    if(*pte & PTE_U)
      kfree((void*)page);
    *pte = 0;
    fileclose(mentry->f);
    memset(mentry, 0, sizeof(struct mmap_struct));
    va+=PGSIZE;
  }
  return 0;
}

// Allocate mmapped page if valid.
// If mapped page is already 
uint64
mmap_fault(pagetable_t pagetable, uint64 va, int read, pte_t* pte)
{
  struct mmap_struct* mv =  &mmap_v[(PTE2PA(*pte))>>12];
  struct file* f = mv->f;
  printf("provided va = %p, provided pte = %p, mv = %p\n", (void*)va, pte, mv );
  printf("mmap fault, file offset = %p, addr = %p\n", (void*)mv->file_offset, (void*)mv->va);
  void* page = kalloc();
  if(page == 0)
    panic("mmap kalloc: out of pages");
  ilock(f->ip);
  int read_size = readi(f->ip, 0, (uint64)page, mv->file_offset, PGSIZE);
  iunlock(f->ip);
  memset(page+read_size, 0, PGSIZE-read_size);
  *pte = PTE_FLAGS(*pte) | PTE_U | PA2PTE(page);
  // vmprint(pagetable);
  return (uint64)page;
}