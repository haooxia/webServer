#include "Server.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>      // NULL
#include <fcntl.h>
#include <string.h>     // memcpy
#include <errno.h>
#include <strings.h>    // strcasecmp
#include <sys/stat.h>   // stat
#include <assert.h>
#include <unistd.h>     // read
#include <sys/sendfile.h>
#include <dirent.h>     // scandir
#include <stdlib.h>     // free
#include <sys/time.h>   // gettimeofday
#include <ctype.h>      // isxdigit
#include <signal.h>
#include <pthread.h>
 
typedef struct FdInfo {
    int fd;
    int epfd;
    pthread_t tid;
}FdInfo;

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
            FdInfo* info = (FdInfo*)malloc(sizeof(FdInfo));
            info->epfd = epfd;
            info->fd = fd;
            if (fd == lfd) {
                // 建立连接：此时不会阻塞哦(内核已经检查过了)
                // 监听操作和通信操作都交给子线程
                // acceptClient(lfd, epfd); // 建立连接会将新fd加到epoll树上，下一轮epoll_wait就会检测了
                pthread_create(&info->tid, NULL, acceptClient, info); // 传出一个tid到info中
            } else {
                // 数据通信: 主要是接收http数据 (write buffer一般都是可用的)
                // recvHttpRequest(fd, epfd);
                pthread_create(&info->tid, NULL, recvHttpRequest, info);
            }
        }
    }
    return 0;
}

// note 10: 修改为多线程版本够应该修改函数原型
// void* acceptClient(void* arg) {
//     FdInfo* info = (FdInfo*)arg;
//     acceptClient(info->lfd, info->epfd);
//     free(info);
//     return NULL;
// } // C可不支持函数重载

// int acceptClient(int lfd, int epfd) {
void* acceptClient(void* arg) {
    FdInfo* info = (FdInfo*)arg;
    // 1. 建立连接
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int cfd = accept(info->fd, (struct sockaddr*)&addr, &addrlen); // 如果无需客户端的信息 指定为NULL即可
    // question1: 为什么一次browser连接会建立两次连接
    printf("browser Socket %d, Address: %s:%d\n", cfd, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    if (cfd == -1) {
        perror("accept");
        return NULL;
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
    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return NULL;
    }
    printf("acceptClient threadID: %ld\n", info->tid);
    free(info); // note 11: 传入参数是堆内存，当对应任务处理完成之后(双方建立连接之后)就可以释放内存了
    return NULL;
}

// int recvHttpRequest(int cfd, int epfd) {
void* recvHttpRequest(void* arg) {
    FdInfo* info = (FdInfo*)arg;
    // 1. 接收数据
    char buf[4096] = {0}; // 搞一块内存用于接受客户端传输来的get请求 (超过4096的不要了)
    char tmp[1024] = {0};
    int len = 0;
    int total = 0;
    while ((len = recv(info->fd, tmp, sizeof(buf), 0)) > 0) {
        if (total + len < sizeof buf) {
            memcpy(buf + total, tmp, len);
        }
        total += len;
    }
    // 2. 判断数据是否接收完毕
    // note2: 如果read buffer中没数据了，我们的recv是非阻塞的，依然会持续读数据，此时返回-1 && errno=EAGAIN
    if (len == -1 && errno == EAGAIN) {
        // printf("HTTP Request Message: \n%s\n", buf);
        // 3. 解析http请求行(GET version)
        char* pt = strstr(buf, "\r\n");
        // 根据请求行长度在尾部添加\0 将http请求报文的*请求行*截取下来
        int reqLen = pt - buf;
        buf[reqLen] = '\0';
        parseRequestLine(buf, info->fd); // 解析并发送http response
    } else if (len == 0) {
        // 4. 客户端断开连接
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
        close(info->fd);
    } else {
        perror("recv");
        return NULL;
    }
    printf("recv message threadID: %ld\n", info->tid);
    free(info);
    return NULL;
}

int parseRequestLine(const char* line, int cfd) {
    // 1. 解析请求行(version不要了) get /xxx/1.jpg http/1.1
    char method[12];
    char path[1024];
    sscanf(line, "%[^ ] %[^ ] ", method, path); // 提取子字符串
    printf("method: %s, path: %s\n", method, path);
    // 忽略get以外的请求
    if (strcasecmp(method, "get") != 0) { // ignoring the case of the characters
        return -1;
    }
    decodeMsg(path, path);
    // printf("method: %s, path_after_decode: %s\n", method, path);

    // 2. 处理客户端请求的静态资源(目录或文件)
    // 需要注意我们在main中已经将服务器路径切换到了资源路径(绝对路径), 后面我们就可以用相对路径操作了 (将http请求中的目录("/形式")切换为我们设置的相对目录)
    char* file = NULL;
    if (strcmp(path, "/") == 0) {
        file = "./"; // "/"就相当于"./""
    } else {
        file = path + 1; // "/xxx"就相当于"xxx"
    }
    // 2.1 获取文件属性
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1) {
        // 文件不存在 --> 回复404 html网页
        sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);
        sendFile("404.html", cfd); // 报错网页就叫404.html, 在资源目录下
        return 0; // 也算是一种成功吧
    }
    // 2.2 判断文件类型
    if (S_ISDIR(st.st_mode)) {
        printf("in dir: %s\n", file);
        // 把本地目录的内容发给客户端(其实就是把遍历目录内的文件和子目录发送出去)
        sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
        sendDir(file, cfd);
    } else {
        printf("file: %s\n", file);
        // 把文件内容发给客户端
        sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
        sendFile(file, cfd);
    }
    return 0;
}

int sendFile(const char* fileName, int cfd) {
    // 打开文件：读一部分，发一部分 (基于TCP)
    // TODO 发多少呢？取决于MSS吗
    int fd = open(fileName, O_RDONLY);
    printf("Start sending...\n");
    assert(fd > 0); // note3: 通过assert判断的条件一般都是严苛的，不允许出错，失败直接挂掉

    struct timeval start, end;
    gettimeofday(&start, NULL);
#if 0 // 方案一：read + send
    long totalBuf = 0;
    long totalLen = 0;
    while (1) {
        char buf[1024]; 
        // 每次都用一个新的buf 
        /* question2 为什么buf的地址每次都一样
           answer: 理论上buf在每一轮循环都是全新的，然而可能是因为编译器优化或者什么原因(待调查)使得每次buf地址一样
           所以strlen(buf)是有内容的，所以一般buf后面会接一个memset，我们这里会直接替换对应地址的值，似乎不memset也没事
        */
        // printf("buf address: %p\n", buf);
        memset(buf, 0, sizeof(buf));
        // printf("sizeof(buf) %ld, strlen(buf) %ld\n", sizeof buf, strlen(buf));
        int len = read(fd, buf, sizeof buf);
        /* note7: 我们期望read每次读取最多sizeof(buf)的数据到buf中(sizeof buf=1024, strlen(buf)=buf中实际存在的字符数)
        question3 然而read并不会每次都读取1024个字符. why?
        */
        totalLen += len;
        totalBuf += strlen(buf);
        printf("len %d, buf %ld...\n", len, strlen(buf));
        // question 4 为什么len和strlen(buf)不一样
        if (len > 0) {
            send(cfd, buf, len, 0);
            printf("send buf %ld, len %d...\n", strlen(buf), len);
            // note4: 服务端传数据给浏览器是比较快的，但浏览器渲染处理啥的可能比较慢，浏览器接受数据的缓存满或来不及处理，所以我们稍微慢点
            usleep(2000); // 微秒; 重要奥，给接收端浏览器喘喘气儿
        } else if (len == 0) {
            break; // 读完了
        } else {
            perror("read");
            return -1;
        }
    }
    printf("total len %ld, total buf %ld\n", totalLen, totalBuf);
#else // 方案二：sendfile
    // note5: 如果通过上面的方式，数据会从用户区拷贝到内核区，费时
    //linux已经给我们提供了一个sendfile函数，可以减少数据拷贝 (号称零拷贝) 适合发送大文件
    
    // struct stat st;
    // stat(fileName, &st);
    // size_t size = st.st_size;

    off_t offset = 0;
    int size = lseek(fd, 0, SEEK_END); // 借助lseek获取文件大小; 注意：这里的fd同事会被移动到文件末尾
    lseek(fd, 0, SEEK_SET); // 将fd移动到文件头部
    // note8: sendfile的发送缓存容量是有限的，文件大小超过该限度是无法写入缓存的，即无法一次性发送到对端，也需要多次发送
    signal(SIGPIPE, SIG_IGN); // 解决mp3无法播放问题
    while (offset < size) {
        int ret = sendfile(cfd, fd, &offset, size); // sendfile很智能啊，会自动修改传入的offset的值，指示现在fd的位置
        // note9: sendfile出现-1是因为cfd被我们设置为非阻塞（正常）TODO 内部机制需要进一步研究；第三个参数也很有用
        if (ret == -1) {
            if (errno != EAGAIN)
                break;
            // printf("没数据...\n"); // sendfile内部不一致大概是 此时数据还没来
            // pass
        }
    }
    printf("here\n");


#endif
    gettimeofday(&end, NULL);
    double duration = (end.tv_sec - start.tv_sec) + 1e-6 * (end.tv_usec - start.tv_usec); 
    printf("End sending, size: %dByte, time duration: %fs!\n", size, duration);
    close(fd);
    return 0;
}

/*
<html>
    <head>
        <title>test</title>
    </head>
    <body>
        <table>
            <tr>
                <td></td>
                <td></td>
            </tr>
            <tr>
                <td></td>
                <td></td>
            </tr>
        </table>
    </body>
</html>
*/

int sendDir(const char* dirName, int cfd) {
    // 将dir list组织成html后再发送给客户端
    char buf[4096] = {0};
    sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);
    struct dirent** nameList; // 指向一个指针数组(若干个dirent*的数组)
    int num = scandir(dirName, &nameList, NULL, alphasort); // 调用scandir内部会malloc得到一块内存 需要free
    if (num == -1) {
        perror("scandir");
        return -1;
    }
    for (int i=0; i<num; ++i) {
        // 取出文件名
        char* name = nameList[i]->d_name;
        char subPath[1024] = {0};
        sprintf(subPath, "%s/%s", dirName, name); // 拼接字符串 为啥不用strcat呢
        struct stat st;
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode)) {
            // 将目录添加到html
            // 添加超链接 <a href="">name</a>
            // sprintf(buf + strlen(buf), "<tr><td>%s</td><td>%ld</td></tr>", name, st.st_size); // 每行显示名称和大小
            sprintf(buf + strlen(buf),
                "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", 
                name, name, st.st_size); // 每行显示子目录 + 名称 + 大小 (%s后面的/代表进入目录)
        } else {
            // 将文件添加到html
            sprintf(buf + strlen(buf),
                "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>", 
                name, name, st.st_size); // 每行显示子目录 + 名称 + 大小 (%s后面没有代表文件)
        }
        send(cfd, buf, strlen(buf), 0);
        // 发送完清空buf中数据
        memset(buf, 0, strlen(buf));
        free(nameList[i]);
    }
    sprintf(buf + strlen(buf), "</table></body></html>");
    send(cfd, buf, strlen(buf), 0);
    free(nameList);
    return 0;
}


int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length) {
    // 1. 状态行
    char buf[4096] = {0};
    // note6: http请求/响应中换行使用\r\n
    sprintf(buf, "http/1.1 %d %s\r\n", status, descr); // 发送格式化输出到指定字符串
    // 2. 响应头
    sprintf(buf + strlen(buf), "content-type: %s\r\n", type);
    sprintf(buf + strlen(buf), "content-length: %d\r\n", length);
    // 3. 空行
    sprintf(buf + strlen(buf), "\r\n");
    // 4. 把header发出去
    send(cfd, buf, strlen(buf), 0);
    return 0;
}

const char* getFileType(const char* name) {
    const char* dot = strrchr(name, '.'); // Find the last occurrence of '.' (from right to left)
    if (dot == NULL) 
        return "text/plain; charset=utf-8"; // 纯文本
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) 
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    return "text/plain; charset=utf-8";
}

// 将字符转换为整数
int hexToDec(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10; // 'a' -> 10 (hex)
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

// 将http编码后的字符串转换为普通字符串
void decodeMsg(char* to, char* from) {
    for (; *from != '\0'; ++from, ++to) {
        if (*from == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            *to = hexToDec(from[1]) * 16 + hexToDec(from[2]);
            from += 2;
        } else {
            *to = *from;
        }
    }
    *to = '\0';
}