#include "kernel/types.h"
#include "user/user.h"

inline unsigned char getdigit(unsigned char byte)
{
    return byte-'0';
}
int get_num(char* str)
{
    int num = 0;
    int dig=getdigit(*str);
    while(dig < 10)
    {
        num = num*10 + dig;
        str++;
        dig = getdigit(*str);
    }

    return num;
}

int main(int argc, char* argv[])
{
    if(argc != 2)
    {
        printf("Usage: %s sleep_timing\n", argv[0]);
        exit(0);
    }
    else
    {
        int time = get_num(argv[1]);
        printf("sleeping for %d\n", time);
        pause(time);
    }
}