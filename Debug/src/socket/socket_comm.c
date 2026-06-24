// Socket 通信工具库，提供客户端与插件之间的数据传输功能

#include "socket_comm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

const char *SOCKET_IP = "127.0.0.1";
const int SOCKET_ACCEPT_TIMEOUT_MS = 5 * 60 * 1000;
const int SOCKET_RECV_TIMEOUT_MS = 5 * 1000;

// 将主机字节序转换为网络字节序
static uint64_t htonll(uint64_t value) {
    const uint32_t test = 1;

    if (*(const unsigned char *)&test == 1) {
        return ((uint64_t)htonl((uint32_t)(value & 0xffffffffu)) << 32) |
               htonl((uint32_t)(value >> 32));
    }

    return value;
}

// 将网络字节序转换为主机字节序
static uint64_t ntohll(uint64_t value) {
    const uint32_t test = 1;

    if (*(const unsigned char *)&test == 1) {
        return ((uint64_t)ntohl((uint32_t)(value & 0xffffffffu)) << 32) |
               ntohl((uint32_t)(value >> 32));
    }

    return value;
}

// 创建基础 socket
static int create_socket(void) {
    return socket(AF_INET, SOCK_STREAM, 0);
}

// 等待 socket 可读，超时返回 -2
static int wait_socket_readable(int fd) {
    struct pollfd pfd;
    int poll_ret;

    if (SOCKET_RECV_TIMEOUT_MS < 0) {
        return 1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    poll_ret = poll(&pfd, 1, SOCKET_RECV_TIMEOUT_MS);
    if (poll_ret == 0) {
        return -2;
    }
    if (poll_ret < 0) {
        return -1;
    }

    return 1;
}

// 关闭 socket
void socket_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

// 关闭一对 socket 资源
void socket_close_resources(int client_fd, int server_fd) {
    if (client_fd >= 0) {
        socket_close(client_fd);
    }

    if (server_fd >= 0) {
        socket_close(server_fd);
    }
}

// 创建监听服务端 socket
int socket_server_listen(uint16_t port) {
    int server_fd;
    struct sockaddr_in addr;

    server_fd = create_socket();
    if (server_fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, SOCKET_IP, &addr.sin_addr) != 1) {
        socket_close(server_fd);
        return -1;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        socket_close(server_fd);
        return -1;
    }

    if (listen(server_fd, 1) < 0) {
        socket_close(server_fd);
        return -1;
    }

    return server_fd;
}

// 接受客户端连接
int socket_server_accept(int server_fd) {
    int client_fd;

    client_fd = accept(server_fd, NULL, NULL);
    return client_fd;
}

// 带超时的接受连接
int socket_server_accept_timeout(int server_fd) {
    struct pollfd pfd;
    int poll_ret;

    if (SOCKET_ACCEPT_TIMEOUT_MS < 0) {
        return socket_server_accept(server_fd);
    }

    pfd.fd = server_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    poll_ret = poll(&pfd, 1, SOCKET_ACCEPT_TIMEOUT_MS);
    if (poll_ret == 0) {
        return -2;
    }
    if (poll_ret < 0) {
        return -1;
    }

    return socket_server_accept(server_fd);
}

// 连接到服务端
int socket_client_connect(uint16_t port) {
    int fd;
    struct sockaddr_in addr;

    fd = create_socket();
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, SOCKET_IP, &addr.sin_addr) != 1) {
        socket_close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        socket_close(fd);
        return -1;
    }

    return fd;
}

// 发送 uint64_t 数据
int socket_send_u64(int fd, uint64_t value) {
    uint64_t net_value;
    const unsigned char *buffer;
    size_t offset = 0;

    net_value = htonll(value);
    buffer = (const unsigned char *)&net_value;
    while (offset < sizeof(net_value)) {
        ssize_t written = send(fd, buffer + offset, sizeof(net_value) - offset, 0);
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }

    return 0;
}

// 发送一个布尔值
int socket_send_bool(int fd, int value) {
    uint8_t net_value;

    net_value = value ? 1u : 0u;
    if (send(fd, &net_value, sizeof(net_value), 0) != (ssize_t)sizeof(net_value)) {
        return -1;
    }

    return 0;
}

// 发送一个 uint8_t 值
int socket_send_u8(int fd, uint8_t value) {
    if (send(fd, &value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
        return -1;
    }

    return 0;
}

// 接收 uint64_t 数据
int socket_recv_u64(int fd, uint64_t *value) {
    unsigned char *buffer;
    size_t offset = 0;

    if (!value) {
        return -1;
    }

    buffer = (unsigned char *)value;
    while (offset < sizeof(*value)) {
        int wait_status = wait_socket_readable(fd);

        if (wait_status < 0) {
            return wait_status;
        }

        ssize_t received = recv(fd, buffer + offset, sizeof(*value) - offset, 0);
        if (received == 0) {
            return 0;
        }
        if (received < 0) {
            return -1;
        }
        offset += (size_t)received;
    }

    if (offset != sizeof(*value)) {
        return -1;
    }

    *value = ntohll(*value);
    return 1;
}

// 接收一个布尔值
int socket_recv_bool(int fd, int *value) {
    uint8_t net_value;
    ssize_t received;

    if (!value) {
        return -1;
    }

    {
        int wait_status = wait_socket_readable(fd);

        if (wait_status < 0) {
            return wait_status;
        }
    }

    received = recv(fd, &net_value, sizeof(net_value), 0);
    if (received == 0) {
        return 0;
    }
    if (received < 0 || received != (ssize_t)sizeof(net_value)) {
        return -1;
    }

    *value = net_value ? 1 : 0;
    return 1;
}

// 接收一个 uint8_t 值
int socket_recv_u8(int fd, uint8_t *value) {
    ssize_t received;

    if (!value) {
        return -1;
    }

    {
        int wait_status = wait_socket_readable(fd);

        if (wait_status < 0) {
            return wait_status;
        }
    }

    received = recv(fd, value, sizeof(*value), 0);
    if (received == 0) {
        return 0;
    }
    if (received < 0 || received != (ssize_t)sizeof(*value)) {
        return -1;
    }

    return 1;
}
