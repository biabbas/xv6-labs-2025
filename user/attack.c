#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
  char* data=sbrk(4096*40);
  for(int i=0;i<40;i++)
    if(strcmp(&data[i*4096+16], "This may help."  ) == 0)
      printf("%s\n", &data[i*4096+16*2]);
}
