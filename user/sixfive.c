#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

inline unsigned char getdigit(unsigned char byte)
{
    return byte-'0';
}

void print_six_five(char* filename)
{
    char buf;
    int fd = open(filename, O_RDONLY);
    int num=0;
    int dig;
    while(read(fd,&buf, 1))
    {
        dig = getdigit(buf);
        if(dig<10)
        {
            num = num*10+dig;
        }
        else if(num != 0)
        {
            if((num%5 == 0) || (num%6 == 0))
                printf("%d\n",num);
            num = 0;
        }
    }
    if(num != 0)
        if((num%5 == 0) || (num%6 == 0))
            printf("%d\n",num);
}
int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s files_to_check_sixfive\n", argv[0]);
        exit(0);
    }

    for (int i=1;i<argc;i++)
        print_six_five(argv[i]);
}