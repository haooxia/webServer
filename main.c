#include <stdio.h>
#include "Server.h"
#include <unistd.h>     // chdir
#include <stdlib.h>     // atoi

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("input format: ./a.out port path\n");
        return -1;
    }
    unsigned short port = atoi(argv[1]);
    // 将当前服务器进程切换到用户指定的资源目录
    chdir(argv[2]);
    // 初始化listen fd
    int lfd = initListenFd(port); // 0-65535
    // 启动服务器程序
    epollRun(lfd);
    
    return 0;
}