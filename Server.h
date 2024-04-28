#pragma once // 等效于ifndef...

// 初始化监听套接字
int initListenFd(unsigned short port);
// 启动epoll
int epollRun(int lfd);
// 与客户端建立连接
int acceptClient(int lfd, int epfd);