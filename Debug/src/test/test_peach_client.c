// 让 Peach 自动重启目标程序功能在测试中可以忽略此文件

#include <stdint.h>
#include <unistd.h>

#include <glib.h>

#include "coverage_comm.h"
#include "socket_comm.h"

static const uint16_t control_socket_port = 2111;
static const uint16_t coverage_socket_port = 3111;

static const uint8_t cmd_shutdown_qemu = 1;
static const uint8_t cmd_restart_service = 2;
static const uint8_t cmd_restart_qemu = 3;

static void send_cmd(int control_fd, uint8_t command, uint8_t *reply) {
    int recv_status;

    if (!reply) {
        return;
    }
    *reply = 0;

    if (socket_send_u8(control_fd, command) < 0) {
        g_message("failed to send control command %u", command);
        return;
    }

    do {
        recv_status = socket_recv_u8(control_fd, reply);
    } while (recv_status == -2);

    if (recv_status <= 0) {
        g_message("failed to receive control reply for command %u", command);
        return;
    }
    if (*reply != 0 && *reply != 1) {
        g_message("invalid control reply %u for command %u", *reply, command);
        return;
    }
}

static int log_hash_count(int round) {
    GHashTable *coverage_map = NULL;

    coverage_comm_load_csv_path = g_strdup_printf("Data/coverage-%d.csv", round);
    coverage_map = coverage_comm_load_coverage_map_from_csv();
    if (!coverage_map) {
        g_message("failed to load coverage CSV for round %d", round);
        g_free(coverage_comm_load_csv_path);
        coverage_comm_load_csv_path = NULL;
        return -1;
    }

    g_message("loaded coverage CSV for round %d: %u records", round, g_hash_table_size(coverage_map));

    g_hash_table_destroy(coverage_map);
    g_free(coverage_comm_load_csv_path);
    coverage_comm_load_csv_path = NULL;
    return 0;
}

static int send_once(int coverage_fd, int round) {
    int recv_value = 0;
    int recv_status;

    if (coverage_fd < 0) {
        g_message("coverage socket %u is not connected", coverage_socket_port);
        return -1;
    }

    if (socket_send_bool(coverage_fd, 1) < 0) {
        g_message("failed to send coverage start flag");
        return -1;
    }

    // 这里的 sleep 仅为占位 实际中应由 Peach 发送协议包
    sleep(5);

    if (socket_send_bool(coverage_fd, 0) < 0) {
        g_message("failed to send coverage end flag");
        return -1;
    }

    recv_status = socket_recv_bool(coverage_fd, &recv_value);
    if (recv_status == -2) {
        g_message("timed out waiting coverage signal in round %d", round);
        return -1;
    }
    if (recv_status <= 0 || recv_value != 1) {
        g_message("unexpected coverage signal in round %d", round);
        return -1;
    }

    // 读取本轮 coverage CSV 并统计 hash 个数
    (void)log_hash_count(round);

    // 占位流量没有真实 SSH 负载 因此固定按模拟超时或失败处理
    g_message("placeholder ssh send is simulated as timeout or failure");
    return -1;
}

// Peach 需要模拟以下行为
// 1 先连接 3111 覆盖率端口
// 2 再连接 2111 控制端口
// 3 等待覆盖率完成通知并读取 CSV 统计 hash
// 4 首次失败时向 2111 发送 2 请求重启服务
// 5 服务恢复失败时向 2111 发送 3 请求重启 QEMU
// 6 QEMU 重启成功后重新建立 3111 连接
// 7 连续失败时向 2111 发送 1 关闭 QEMU 后退出
int main(void) {
    int control_fd = -1;
    int coverage_fd = -1;
    int coverage_round = 0;
    int qemu_restarted = 0;
    uint8_t reply = 0;

    // 1 先连接 3111 覆盖率端口用于后续多轮采集
    coverage_fd = socket_client_connect(coverage_socket_port);
    while (coverage_fd < 0) {
        sleep(1);
        coverage_fd = socket_client_connect(coverage_socket_port);
    }
    g_message("connected coverage socket %u", coverage_socket_port);

    // 2 再连接 2111 控制端口 失败则持续重试
    control_fd = socket_client_connect(control_socket_port);
    while (control_fd < 0) {
        sleep(1);
        control_fd = socket_client_connect(control_socket_port);
    }
    g_message("connected control socket %u", control_socket_port);

    // 3 首次发送占位流量并获取覆盖率 hash 个数
    coverage_round++;
    if (send_once(coverage_fd, coverage_round) == 0) {
        if (coverage_fd >= 0) {
            socket_close(coverage_fd);
        }
        socket_close(control_fd);
        return 0;
    }

    // 4 首次失败后请求重启服务
    g_message("first send failed or timed out, request service restart");
    send_cmd(control_fd, cmd_restart_service, &reply);

    // 5 服务重启回复为 1 时再发送一次并获取覆盖率 hash 个数
    if (reply == 1) {
        g_message("service restart reply is 1, retry send once");
        coverage_round++;
        if (send_once(coverage_fd, coverage_round) != 0) {
            g_message("retry after service restart still failed, request qemu restart");
            if (coverage_fd >= 0) {
                socket_close(coverage_fd);
                coverage_fd = -1;
            }
            send_cmd(control_fd, cmd_restart_qemu, &reply);
            if (reply == 1) {
                qemu_restarted = 1;
                coverage_round = 0;
            }
        } else {
            if (coverage_fd >= 0) {
                socket_close(coverage_fd);
            }
            socket_close(control_fd);
            return 0;
        }
    } else {
        // 5 服务重启回复为 0 时直接请求重启 QEMU
        g_message("service restart reply is 0, request qemu restart");
        if (coverage_fd >= 0) {
            socket_close(coverage_fd);
            coverage_fd = -1;
        }
        send_cmd(control_fd, cmd_restart_qemu, &reply);
        if (reply == 1) {
            qemu_restarted = 1;
            coverage_round = 0;
        }
    }

    // 6 QEMU 重启成功后重新建立 3111 连接
    if (qemu_restarted) {
        coverage_fd = socket_client_connect(coverage_socket_port);
        if (coverage_fd < 0) {
            g_message("failed to reconnect coverage socket %u after qemu restart", coverage_socket_port);
            send_cmd(control_fd, cmd_shutdown_qemu, &reply);
            socket_close(control_fd);
            return 1;
        }
        g_message("reconnected coverage socket %u after qemu restart", coverage_socket_port);
    }

    // 7 最后一次发送并获取覆盖率 hash 个数 仍失败则关闭 QEMU 退出
    coverage_round++;
    if (send_once(coverage_fd, coverage_round) != 0) {
        g_message("still failed after qemu restart, send shutdown and exit");
        send_cmd(control_fd, cmd_shutdown_qemu, &reply);
        if (coverage_fd >= 0) {
            socket_close(coverage_fd);
        }
        socket_close(control_fd);
        return 1;
    }

    if (coverage_fd >= 0) {
        socket_close(coverage_fd);
    }
    socket_close(control_fd);
    return 0;
}
