#include "Server.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h> // NULL
#include <fcntl.h>

int initListenFd(unsigned short port) {
    // 1. 创建监听fd
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket");
        return -1;
    }
    // 2. 设置端口复用(可省略)
    int opt = 1; // 主动断开连接的一方需要1min才能释放端口(2msl)
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret == -1) {
        perror("setsocket");
        return -1;
    }
    // 3. 绑定端口
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY; // 0
    ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == -1) {
        perror("bind");
        return -1;
    }
    // 4. 设置监听
    ret = listen(lfd, 128);
    if (ret == -1) {
        perror("listen");
        return -1;
    }
    // 5. 返回lfd
    return lfd;
}

int epollRun(int lfd) {
    // 1. 创建epoll实例
    int epfd = epoll_create(1); // epfd is also a file descriptor
    if (epfd == -1) {
        perror("epoll_create");
        return -1;
    }
    // 2. 将监听lfd添加到红黑树
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return -1;
    }
    // 3. 持续检测有无事件被激活
    struct epoll_event evs[1024];
    while (1) {
        int num = epoll_wait(epfd, evs, sizeof(evs)/sizeof(evs[0]), -1); // "events" parameter is a buffer that will contain triggered events.
        for (int i = 0; i < num; ++i) {
            int fd = evs[i].data.fd;
            if (fd == lfd) {
                // 建立连接：此时不会阻塞哦(内核已经检查过了)
                acceptClient(lfd, epfd); // 建立连接会将新fd加到epoll树上，下一轮epoll_wait就会检测了
                
            } else {
                // 数据通信: 主要是接收http数据
                
            }
        }
    }
    return 0;
}

int acceptClient(int lfd, int epfd) {
    // 1. 建立连接
    int cfd = accept(lfd, NULL, NULL); // 如果无需客户端的信息 指定为NULL即可
    if (cfd == -1) {
        perror("accept");
        return -1;
    }
    // 2. 设置非阻塞
    // note 1: epoll的ET非阻塞模式效率最高
    int flag = fcntl(cfd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);
    // 3. 将cfd添加到epoll红黑树
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET; // 设置ET模式
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return -1;
    }
    return 0;
}