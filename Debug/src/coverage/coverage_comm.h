#ifndef COVERAGE_COMM_H
#define COVERAGE_COMM_H

#include <stdio.h>
#include <stdint.h>
#include <glib.h>

typedef struct {
    uint64_t start_addr;
    uint64_t trans_count;
    uint64_t exec_count;
    uint64_t insn_count;
} Coverage;

typedef struct {
    FILE *fp;
    const char *path;
    GMutex lock;
} CoverageCommLogContext;

extern CoverageCommLogContext coverage_comm_log_ctx;
extern char *coverage_comm_log_path;
extern char *coverage_comm_csv_path;
extern char *coverage_comm_load_csv_path;

uint64_t record_hash(uint64_t start_addr, uint64_t insn_count);
GList *coverage_comm_top_records(GHashTable *coverage_map, GMutex *lock);
void coverage_comm_log_top_records(const char *title, GHashTable *coverage_map, GMutex *lock, guint64 limit);
void coverage_comm_log_init(void);
void coverage_comm_log_open(void);
void coverage_comm_log_close(void);
void coverage_comm_glib_log_handler(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer user_data);
guint64 coverage_comm_dump_coverage_map_to_csv(GHashTable *coverage_map, GMutex *lock);
GHashTable *coverage_comm_load_coverage_map_from_csv(void);

#endif
