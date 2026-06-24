// QEMU 覆盖率客户端测试程序，通过 socket 驱动插件进行多轮覆盖率采集

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#include <glib.h>

#include "coverage_comm.h"
#include "socket_comm.h"

static const uint16_t coverage_socket_port = 3111;

// 客户端按轮次接收 coverage CSV
static int run_round(int fd, int round) {
    GHashTable *coverage_map = NULL;
    int recv_value = 0;
    int recv_status;

    if (socket_send_bool(fd, 1) < 0) {
        g_critical("failed to send start flag for round %d", round);
        return -1;
    }

    sleep(5);

    if (socket_send_bool(fd, 0) < 0) {
        g_critical("failed to send end flag for round %d", round);
        return -1;
    }

    recv_status = socket_recv_bool(fd, &recv_value);
    if (recv_status == -2) {
        g_warning("timed out waiting for coverage signal in round %d", round);
        return -1;
    }

    if (recv_status <= 0 || recv_value != 1) {
        if (recv_status < 0) {
            g_critical("failed to receive coverage signal in round %d", round);
        } else {
            g_warning("unexpected coverage signal in round %d", round);
        }
        return -1;
    }

    coverage_comm_load_csv_path = g_strdup_printf("Data/coverage-%d.csv", round);
    coverage_map = coverage_comm_load_coverage_map_from_csv();
    if (!coverage_map) {
        g_critical("failed to load coverage CSV for round %d", round);
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

int main(void) {
    int fd = -1;

    fd = socket_client_connect(coverage_socket_port);
    if (fd < 0) {
        g_critical("connect failed");
        return 1;
    }

    if (run_round(fd, 1) < 0 || run_round(fd, 2) < 0) {
        socket_close(fd);
        return 1;
    }

    socket_close(fd);
    return 0;
}
