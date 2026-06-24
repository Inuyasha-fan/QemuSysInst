#ifndef SOCKET_COMM_H
#define SOCKET_COMM_H

#include <stdint.h>

// socket 默认配置
extern const char *SOCKET_IP;
extern const int SOCKET_ACCEPT_TIMEOUT_MS;
// 接收数据的超时时间，单位毫秒，负数时关闭超时模式
extern const int SOCKET_RECV_TIMEOUT_MS;

// 创建并监听服务端 socket
int socket_server_listen(uint16_t port);
// 接受客户端连接
int socket_server_accept(int server_fd);
// 带超时的客户端连接接收，超时返回 -2
int socket_server_accept_timeout(int server_fd);
// 连接到服务端 socket
int socket_client_connect(uint16_t port);
// 发送一个 uint64_t 值
int socket_send_u64(int fd, uint64_t value);
// 发送一个 uint8_t 值
int socket_send_u8(int fd, uint8_t value);
// 发送一个布尔值
int socket_send_bool(int fd, int value);
// 接收一个 uint64_t 值，返回 1 表示成功，0 表示对端关闭，-2 表示超时，-1 表示错误
int socket_recv_u64(int fd, uint64_t *value);
// 接收一个 uint8_t 值，返回 1 表示成功，0 表示对端关闭，-2 表示超时，-1 表示错误
int socket_recv_u8(int fd, uint8_t *value);
// 接收一个布尔值，返回 1 表示成功，0 表示对端关闭，-2 表示超时，-1 表示错误
int socket_recv_bool(int fd, int *value);
// 关闭 socket
void socket_close(int fd);
// 关闭一对 socket 资源
void socket_close_resources(int client_fd, int server_fd);

#endif
