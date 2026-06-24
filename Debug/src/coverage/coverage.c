// QEMU 覆盖率插件主文件，负责收集指令执行哈希并写入 CSV 文件

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <qemu-plugin.h>

#include "coverage_comm.h"
#include "socket_comm.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static bool do_inline;
static const char *log_path = "./Log/coverage.log";
static const char *data_dir_path = "./Data";
static const char *noise_csv_filename = "noise.csv";
static const char *coverage_csv_filename_format = "coverage-%" PRIu64 ".csv";
static uint16_t socket_port = 3111;

static GMutex lock;
static GMutex socket_lock;
static GHashTable *noise_map;
static GHashTable *coverage_map;
static GThread *socket_server_thread;
static gint socket_server_fd = -1;
static gint socket_client_fd = -1;
static gint collecting_noise;
static gint collecting_coverage;
static gint shutdown_requested;
static gint shutdown_notified;
static guint64 coverage_round;

static gboolean parse_socket_port(const char *value, uint16_t *parsed_port) {
    char *end = NULL;
    guint64 parsed;

    if (!value || !parsed_port || value[0] == '\0') {
        return FALSE;
    }

    parsed = g_ascii_strtoull(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        return FALSE;
    }
    if (parsed == 0 || parsed > UINT16_MAX) {
        return FALSE;
    }

    *parsed_port = (uint16_t)parsed;
    return TRUE;
}

// 状态控制
// 请求所有后台线程退出
static void request_shutdown(void) {
    g_atomic_int_set(&shutdown_requested, TRUE);
    g_atomic_int_set(&collecting_noise, FALSE);
    g_atomic_int_set(&collecting_coverage, FALSE);

    if (!g_atomic_int_compare_and_exchange(&shutdown_notified, FALSE, TRUE)) {
        return;
    }
}

// 向当前客户端发送一个覆盖率完成通知
static int notify_coverage_written(void) {
    gint client_fd;

    g_mutex_lock(&socket_lock);
    client_fd = socket_client_fd;
    g_mutex_unlock(&socket_lock);

    if (client_fd < 0) {
        return -1;
    }

    if (socket_send_bool(client_fd, 1) < 0) {
        request_shutdown();
        return -1;
    }

    return 0;
}

// socket 资源

// 关闭 socket 资源并清理连接状态
static void close_socket_state(void) {
    gint client_fd;
    gint server_fd;

    g_mutex_lock(&socket_lock);
    client_fd = socket_client_fd;
    server_fd = socket_server_fd;
    socket_client_fd = -1;
    socket_server_fd = -1;
    g_mutex_unlock(&socket_lock);

    socket_close_resources(client_fd, server_fd);
}

// 过滤和队列

// 如果 hash 在 noise 表中出现过，则跳过
static gboolean noise_contains_hash(uint64_t hash) {
    gboolean found;

    if (!noise_map) {
        return FALSE;
    }

    g_mutex_lock(&lock);
    found = g_hash_table_contains(noise_map, (gconstpointer)(uintptr_t)hash);
    g_mutex_unlock(&lock);
    return found;
}

// 在指定 map 中创建或更新 TB 记录
static Coverage *record_tb(GHashTable *map, uint64_t hash, uint64_t start_addr, uint64_t insn_count) {
    Coverage *coverage;

    g_mutex_lock(&lock);
    coverage = g_hash_table_lookup(map, (gconstpointer)hash);
    if (coverage) {
        coverage->trans_count++;
    } else {
        coverage = g_new0(Coverage, 1);
        coverage->start_addr = start_addr;
        coverage->trans_count = 1;
        coverage->insn_count = insn_count;
        g_hash_table_insert(map, (gpointer)(uintptr_t)hash, coverage);
    }
    g_mutex_unlock(&lock);

    return coverage;
}

// 阶段切换

// 结束 noise 阶段，写 noise.csv 并切到 coverage 阶段
static void start_coverage_phase(void) {
    char *noise_csv_path;

    // noise 结果只导出一次，后续 coverage 使用独立路径
    noise_csv_path = g_build_filename(data_dir_path, noise_csv_filename, NULL);
    g_free(coverage_comm_csv_path);
    coverage_comm_csv_path = noise_csv_path;
    coverage_comm_dump_coverage_map_to_csv(noise_map, &lock);
    g_atomic_int_set(&collecting_noise, FALSE);
    g_atomic_int_set(&collecting_coverage, TRUE);
}

// socket 服务器线程

// 接收开始/停止控制并切换 noise / coverage 阶段
static gpointer socket_server(gpointer unused) {
    gboolean accepted = FALSE;

    socket_server_fd = socket_server_listen(socket_port);
    if (socket_server_fd < 0) {
        g_critical("Failed to create socket server.");
        request_shutdown();
        return NULL;
    }

    g_mutex_lock(&socket_lock);
    socket_client_fd = socket_server_accept_timeout(socket_server_fd);
    g_mutex_unlock(&socket_lock);

    if (socket_client_fd == -2) {
        g_critical("Socket accept timed out.");
        request_shutdown();
    } else if (socket_client_fd < 0) {
        g_critical("Failed to accept socket client.");
        request_shutdown();
    } else {
        accepted = TRUE;
        g_message("Socket client connected successfully.");
    }

    if (accepted) {
        while (!g_atomic_int_get(&shutdown_requested)) {
            int value;
            int ret;
            gint client_fd;

            g_mutex_lock(&socket_lock);
            client_fd = socket_client_fd;
            g_mutex_unlock(&socket_lock);

            ret = socket_recv_bool(client_fd, &value);
            if (ret == 1) {
                if (value == 1) {
                    // 1 表示从 noise 切到 coverage，或者重新开始 coverage
                    if (g_atomic_int_get(&collecting_noise)) {
                        g_message("Noise stop received.");
                        start_coverage_phase();
                    } else if (!g_atomic_int_get(&collecting_coverage)) {
                        g_message("Coverage start received.");
                        g_atomic_int_set(&collecting_coverage, TRUE);
                    }
                } else {
                    // 0 表示当前 coverage 轮结束，立即落盘
                    char *coverage_csv_filename;
                    char *coverage_csv_path;

                    g_atomic_int_set(&collecting_coverage, FALSE);
                    g_message("Coverage stop received.");
                    coverage_round++;
                    coverage_csv_filename = g_strdup_printf(coverage_csv_filename_format, coverage_round);
                    coverage_csv_path = g_build_filename(data_dir_path, coverage_csv_filename, NULL);
                    g_free(coverage_csv_filename);
                    g_free(coverage_comm_csv_path);
                    coverage_comm_csv_path = coverage_csv_path;
                    g_message("Coverage round %" PRIu64 " start writing.", coverage_round);
                    coverage_comm_log_top_records("Coverage", coverage_map, &lock, 10);
                    coverage_comm_dump_coverage_map_to_csv(coverage_map, &lock);
                    if (notify_coverage_written() == 0) {
                        g_message("Coverage round %" PRIu64 " notification sent.", coverage_round);
                    } else {
                        g_warning("Coverage round %" PRIu64 " notification failed.", coverage_round);
                    }
                    g_message("Coverage round %" PRIu64 " writing done.", coverage_round);
                    g_mutex_lock(&lock);
                    g_hash_table_remove_all(coverage_map);
                    g_mutex_unlock(&lock);
                }
            } else if (ret == -2) {
                continue;
            } else {
                g_message("Stop receiving socket control message.");
                request_shutdown();
                break;
            }
        }
    }

    close_socket_state();
    return NULL;
}

// 生命周期

// 插件初始化时清理 Data 目录下的 csv 文件
static void cleanup_data_csv_files(void) {
    GDir *data_dir = g_dir_open(data_dir_path, 0, NULL);
    const gchar *entry_name;

    if (!data_dir) {
        g_warning("Failed to open %s for CSV cleanup.", data_dir_path);
        return;
    }

    while ((entry_name = g_dir_read_name(data_dir)) != NULL) {
        char *entry_path;

        if (!g_str_has_suffix(entry_name, ".csv")) {
            continue;
        }

        entry_path = g_build_filename(data_dir_path, entry_name, NULL);
        if (g_remove(entry_path) == 0) {
            g_message("Removed stale CSV: %s", entry_path);
        } else {
            g_warning("Failed to remove CSV: %s", entry_path);
        }
        g_free(entry_path);
    }

    g_dir_close(data_dir);
}

// 插件退出时等待线程并释放资源
static void plugin_exit(qemu_plugin_id_t id G_GNUC_UNUSED, void *p G_GNUC_UNUSED) {
    request_shutdown();
    close_socket_state();
    g_message("Coverage plugin exit started.");

    if (socket_server_thread) {
        g_thread_join(socket_server_thread);
        socket_server_thread = NULL;
    }
    if (coverage_map) {
        g_hash_table_destroy(coverage_map);
        coverage_map = NULL;
    }
    if (noise_map) {
        g_hash_table_destroy(noise_map);
        noise_map = NULL;
    }

    g_message("Coverage plugin exit done.");
    coverage_comm_log_close();
}

// 初始化日志、过滤表、共享哈希表和后台线程
static void plugin_init(void) {
    cleanup_data_csv_files();

    coverage_comm_log_path = (char *)log_path;
    coverage_comm_log_init();
    coverage_comm_log_open();
    g_message("Coverage control socket port: %" PRIu16 ".", socket_port);

    coverage_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    noise_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    g_atomic_int_set(&collecting_noise, TRUE);
    g_atomic_int_set(&collecting_coverage, FALSE);
    g_atomic_int_set(&shutdown_requested, FALSE);
    g_atomic_int_set(&shutdown_notified, FALSE);

    g_mutex_init(&socket_lock);
    socket_client_fd = -1;
    socket_server_fd = -1;

    socket_server_thread = g_thread_new("socket-server", socket_server, NULL);
}

// TB 回调

// TB 执行回调：累计 noise 阶段 exec_count
static void vcpu_tb_exec_noise(unsigned int cpu_index G_GNUC_UNUSED, void *udata) {
    uint64_t hash = (uint64_t)udata;
    Coverage *coverage;

    // noise 阶段只需要统计命中次数，不发送给客户端
    g_mutex_lock(&lock);
    coverage = g_hash_table_lookup(noise_map, (gconstpointer)hash);
    g_assert(coverage);
    coverage->exec_count++;
    g_mutex_unlock(&lock);
}

// TB 执行回调：累计 coverage 阶段 exec_count
static void vcpu_tb_exec_coverage(unsigned int cpu_index G_GNUC_UNUSED, void *udata) {
    uint64_t hash = (uint64_t)udata;
    Coverage *coverage;

    if (!g_atomic_int_get(&collecting_coverage)) {
        return;
    }

    // coverage 阶段要保留执行次数
    g_mutex_lock(&lock);
    coverage = g_hash_table_lookup(coverage_map, (gconstpointer)hash);
    if (!coverage) {
        coverage = g_new0(Coverage, 1);
        coverage->trans_count = 1;
        g_hash_table_insert(coverage_map, (gpointer)(uintptr_t)hash, coverage);
    }
    coverage->exec_count++;
    g_mutex_unlock(&lock);
}

// TB 转换回调：在 noise 阶段填 noise_map，在 coverage 阶段填 coverage_map
static void vcpu_tb_trans(qemu_plugin_id_t id G_GNUC_UNUSED, struct qemu_plugin_tb *tb) {
    uint64_t start_addr = qemu_plugin_tb_vaddr(tb);
    uint64_t insn_count = qemu_plugin_tb_n_insns(tb);
    uint64_t hash = record_hash(start_addr, insn_count);
    Coverage *coverage;

    if (g_atomic_int_get(&collecting_noise)) {
        // noise 阶段保留所有 TB，供后续 coverage 过滤
        coverage = record_tb(noise_map, hash, start_addr, insn_count);
        if (do_inline) {
            qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &coverage->exec_count, 1);
        } else {
            qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_noise, QEMU_PLUGIN_CB_NO_REGS, (void *)hash);
        }
        return;
    }

    if (!g_atomic_int_get(&collecting_coverage) || noise_contains_hash(hash)) {
        return;
    }

    // coverage 阶段只记录 noise 阶段没有出现过的 TB
    coverage = record_tb(coverage_map, hash, start_addr, insn_count);
    if (do_inline) {
        qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &coverage->exec_count, 1);
    } else {
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_coverage, QEMU_PLUGIN_CB_NO_REGS, (void *)hash);
    }
}

// 插件入口
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info G_GNUC_UNUSED, int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);
        if (g_strcmp0(tokens[0], "inline") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &do_inline)) {
                g_critical("Boolean argument parsing failed: %s", opt);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "port") == 0) {
            uint16_t parsed_port;

            if (!parse_socket_port(tokens[1], &parsed_port)) {
                g_critical("Port argument parsing failed: %s", opt);
                return -1;
            }
            socket_port = parsed_port;
        } else {
            g_critical("Option parsing failed: %s", opt);
            return -1;
        }
    }

    plugin_init();
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
