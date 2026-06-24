// 覆盖率通信工具库，提供哈希计算、CSV 读写和覆盖率数据处理功能

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "coverage_comm.h"

CoverageCommLogContext coverage_comm_log_ctx;
char *coverage_comm_log_path = NULL;
char *coverage_comm_csv_path = NULL;
char *coverage_comm_load_csv_path = NULL;

uint64_t record_hash(uint64_t start_addr, uint64_t insn_count) {
    uint64_t x = start_addr + 0x9e3779b97f4a7c15ULL;
    uint64_t y = insn_count + 0xbf58476d1ce4e5b9ULL;

    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;

    y ^= y >> 30;
    y *= 0xbf58476d1ce4e5b9ULL;
    y ^= y >> 27;
    y *= 0x94d049bb133111ebULL;
    y ^= y >> 31;

    return x ^ (y + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2));
}

static const char *log_level_to_string(GLogLevelFlags level) {
    if (level & G_LOG_LEVEL_CRITICAL) {
        return "CRITICAL";
    }
    if (level & G_LOG_LEVEL_WARNING) {
        return "WARNING";
    }
    if (level & G_LOG_LEVEL_INFO) {
        return "INFO";
    }
    if (level & G_LOG_LEVEL_MESSAGE) {
        return "MESSAGE";
    }
    if (level & G_LOG_LEVEL_DEBUG) {
        return "DEBUG";
    }
    return "LOG";
}

static gint cmp_exec_count(gconstpointer a, gconstpointer b) {
    const Coverage *left = a;
    const Coverage *right = b;

    if (left->exec_count == right->exec_count) {
        return 0;
    }

    return left->exec_count > right->exec_count ? -1 : 1;
}

void coverage_comm_log_init(void) {
    coverage_comm_log_ctx.fp = NULL;
    coverage_comm_log_ctx.path = coverage_comm_log_path;
    g_mutex_init(&coverage_comm_log_ctx.lock);
}

void coverage_comm_glib_log_handler(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer user_data) {
    CoverageCommLogContext *ctx = user_data;
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *timestamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    FILE *fp;

    if (!ctx) {
        return;
    }

    fp = ctx->fp ? ctx->fp : stderr;

    g_mutex_lock(&ctx->lock);
    if (domain && *domain) {
        fprintf(fp, "%s [%s] %s: %s\n",
                timestamp ? timestamp : "unknown-time",
                log_level_to_string(level),
                domain,
                message ? message : "");
    } else {
        fprintf(fp, "%s [%s] %s\n",
                timestamp ? timestamp : "unknown-time",
                log_level_to_string(level),
                message ? message : "");
    }
    fflush(fp);
    g_mutex_unlock(&ctx->lock);
}

void coverage_comm_log_open(void) {
    coverage_comm_log_ctx.fp = fopen(coverage_comm_log_ctx.path, "w");
    if (!coverage_comm_log_ctx.fp) {
        coverage_comm_log_ctx.fp = stderr;
    }
    g_log_set_default_handler(coverage_comm_glib_log_handler, &coverage_comm_log_ctx);
}

void coverage_comm_log_close(void) {
    g_log_set_default_handler(g_log_default_handler, NULL);

    g_mutex_lock(&coverage_comm_log_ctx.lock);
    if (coverage_comm_log_ctx.fp && coverage_comm_log_ctx.fp != stderr) {
        fclose(coverage_comm_log_ctx.fp);
    }
    coverage_comm_log_ctx.fp = NULL;
    g_mutex_unlock(&coverage_comm_log_ctx.lock);
    g_mutex_clear(&coverage_comm_log_ctx.lock);
}

GList *coverage_comm_top_records(GHashTable *coverage_map, GMutex *lock) {
    GList *records;
    GList *snapshot;

    g_mutex_lock(lock);
    if (!coverage_map || g_hash_table_size(coverage_map) == 0) {
        g_mutex_unlock(lock);
        return NULL;
    }

    records = g_hash_table_get_values(coverage_map);
    g_mutex_unlock(lock);

    snapshot = g_list_sort(records, cmp_exec_count);
    return snapshot;
}

void coverage_comm_log_top_records(const char *title, GHashTable *coverage_map, GMutex *lock, guint64 limit) {
    GList *records;
    GList *iter;
    guint64 i = 0;

    records = coverage_comm_top_records(coverage_map, lock);
    if (!records) {
        return;
    }

    g_message("%s top records:", title);
    for (iter = records; iter && i < limit; iter = iter->next, i++) {
        Coverage *rec = iter->data;
        g_message("0x%016" PRIx64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64,
                  rec->start_addr, rec->trans_count, rec->exec_count, rec->insn_count);
    }
    g_list_free(records);
}

static gboolean parse_u64_field(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (!text) {
        return FALSE;
    }

    while (g_ascii_isspace(*text)) {
        text++;
    }

    if (*text == '\0') {
        return FALSE;
    }

    parsed = g_ascii_strtoull(text, &end, 0);
    if (end == text) {
        return FALSE;
    }

    while (end && g_ascii_isspace(*end)) {
        end++;
    }

    if (end && *end != '\0') {
        return FALSE;
    }

    *value = (uint64_t)parsed;
    return TRUE;
}

guint64 coverage_comm_dump_coverage_map_to_csv(GHashTable *coverage_map, GMutex *lock) {
    FILE *fp;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    guint64 written_count = 0;

    if (!coverage_comm_csv_path) {
        return 0;
    }

    fp = fopen(coverage_comm_csv_path, "w");
    if (!fp) {
        return 0;
    }

    fprintf(fp, "hash, start_addr, trans_count, exec_count, insn_count\n");

    g_mutex_lock(lock);
    if (coverage_map) {
        g_hash_table_iter_init(&iter, coverage_map);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            Coverage *record = (Coverage *)value;
            uint64_t hash = (uint64_t)(uintptr_t)key;

            if (record->exec_count == 0) {
                continue;
            }

            fprintf(fp, "0x%016" PRIx64 ", 0x%016" PRIx64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
                    hash,
                    record->start_addr,
                    record->trans_count,
                    record->exec_count,
                    record->insn_count);
            written_count++;
        }
    }
    g_mutex_unlock(lock);

    fflush(fp);
    fclose(fp);
    return written_count;
}

GHashTable *coverage_comm_load_coverage_map_from_csv(void) {
    FILE *fp;
    char line[512];
    GHashTable *coverage_map;

    if (!coverage_comm_load_csv_path) {
        return NULL;
    }

    fp = fopen(coverage_comm_load_csv_path, "r");
    if (!fp) {
        return NULL;
    }

    coverage_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    while (fgets(line, sizeof(line), fp)) {
        gchar **fields;
        uint64_t hash;
        uint64_t start_addr;
        uint64_t trans_count;
        uint64_t exec_count;
        uint64_t insn_count;
        Coverage *record;

        if (line[0] == '\0' || line[0] == '\n') {
            continue;
        }

        fields = g_strsplit(line, ",", -1);
        if (!fields[0] || !fields[1] || !fields[2] || !fields[3] || !fields[4]) {
            g_strfreev(fields);
            continue;
        }

        if (g_str_has_prefix(fields[0], "hash")) {
            g_strfreev(fields);
            continue;
        }

        if (!parse_u64_field(fields[0], &hash) ||
            !parse_u64_field(fields[1], &start_addr) ||
            !parse_u64_field(fields[2], &trans_count) ||
            !parse_u64_field(fields[3], &exec_count) ||
            !parse_u64_field(fields[4], &insn_count)) {
            g_strfreev(fields);
            continue;
        }

        record = g_new0(Coverage, 1);
        record->start_addr = start_addr;
        record->trans_count = trans_count;
        record->exec_count = exec_count;
        record->insn_count = insn_count;
        g_hash_table_insert(coverage_map, (gpointer)(uintptr_t)hash, record);
        g_strfreev(fields);
    }

    fclose(fp);
    return coverage_map;
}
