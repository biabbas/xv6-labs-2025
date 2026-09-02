typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;

typedef uint64 pde_t;
#ifdef LAB_MMAP
typedef unsigned long size_t;
#ifndef __APPLE__
typedef long int off_t;
#endif
#endif
