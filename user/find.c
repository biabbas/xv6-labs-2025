#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#define BUF_SIZE 512

inline char* leaf_name(char* p)
{
    char* last_slash=0;
    while(*p)
    {
        if(*p++ == '/')
         last_slash = p;
    }
    return last_slash;
}
void find(char* path, char* look_path, void (*exec_fn)(char*), int path_len)
{
    int fd;
    struct stat st;
    struct dirent de;
    char* p;
    if(stat(path, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        return;
    }
    if((look_path==0) || (strcmp(path, look_path)==0) || (strcmp(leaf_name(path), look_path)==0))
        exec_fn(path);
    if(st.type == T_DIR)
    {
        if((fd = open(path, O_RDONLY)) < 0) {
            fprintf(2, "find: cannot open %s\n", path);
            return;
        }
        p = &path[path_len];
        *p++ = '/';
        while(read(fd, &de, sizeof(struct dirent)) == sizeof(struct dirent))
        {
            if(de.inum == 0)
             continue;
            int diff = strcmp("..", de.name);
            if((diff == 0) || (diff == (int)'.'))
             continue;
            memmove(p, de.name, DIRSIZ);
            p[DIRSIZ] = 0;
            find(path, look_path, exec_fn, strlen(path));

        }
    }
}

void print_str(char* name)
{
    printf("%s\n",name);
}

char **exec_args=0;
int exec_arg_index=0;
void fork_exec(char* arg)
{
    int status;
    if(fork() == 0)
    {
        exec_args[exec_arg_index] = arg;
        exec(exec_args[0], exec_args);
    }
    else
        wait(&status);
}
int main(int argc, char* argv[])
{
    char buf[BUF_SIZE];
    int len;
    
    if(argc == 2)
    {
        len = strlen(argv[1]);
        if(len > BUF_SIZE)
        {
            fprintf(2, "find: path too long\n");
            exit(1);
        }
        strcpy(buf, argv[1]);
        buf[len] = 0;
        find(buf, 0, &print_str, len);
    }
    else if(argc == 3)
    {
        len = strlen(argv[1]);
        if(len > BUF_SIZE)
        {
            fprintf(2, "find: path too long\n");
            exit(1);
        }
        strcpy(buf, argv[1]);
        buf[len] = 0;
        find(buf, argv[2], &print_str, len);
    }
    else if ( (argc > 4) && (strcmp(argv[3], "-exec")==0) )
    {
        exec_args = (char**)malloc((argc-4+2)*sizeof(char*));
        for(int i=4;i<argc; i++)
            exec_args[exec_arg_index++]=argv[i];
        exec_args[exec_arg_index+1]=0;
        len = strlen(argv[1]);
        if(len > BUF_SIZE)
        {
            fprintf(2, "find: path too long\n");
            exit(1);
        }
        strcpy(buf, argv[1]);
        buf[len] = 0;
        find(buf, argv[2], &fork_exec, len);
    }
    else
    {
        fprintf(2, "Usage: %s directory_to_find [optional](-exec command)\n",argv[0]);
        exit(1);
    }

}