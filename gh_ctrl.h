#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include "sqlite3.h"

#define GHPI_SQLITE_INIT_STRING "\
pragma journal_mode = WAL; \
pragma synchronous = normal; \
pragma foreign_keys = on; \
pragma recursive_triggers = true; \
pragma cache_size = -256; \
pragma secure_delete = false;"

#define TS_TV_NSEC_SHIFT 23 // To quickly convert ts.tv_nsec approximately to centiseconds
#define HYST_SCALE 4 // For hysteresis of sensor readings
#define GHPI_SQL_BUSY_WAIT 100*1000000 // ns. I.e. 100ms.
#define GHPI_SQL_BUSY_RETRY_MAX 50
#define GHPI_SQL_BUSY_NOTICE_THRESHOLD 1
#define TIMESTAMP_RETRY_MAX 20

// Due to the anti-joy of C.
// Cribbed from https://stackoverflow.com/questions/3437404/min-and-max-in-c
// Double evaluation prevention not necessary in this program, but still...
#define max(a,b) \
  ({ __typeof__ (a) _a = (a); \
      __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b; })

extern sqlite3* db;
extern sqlite3_stmt* pStmt_log;
extern const char* Sqlite_type_names[];
extern const char* sensor_type;
extern const char* zSql_log;

void ts_diff(struct timespec* result, struct timespec* a, struct timespec* b);
bool reading_change(bool* px_prev_inc_p, int* px_mem, int x);
void check_sql(int rc, const char* msg);
int ghpi_sqlite_busy_handler(void* argv0, int count);
void ghpi_sqlite_init(const char* argv0, const char* db_file_name);
void daemon_init(int argc, char** argv);
void insert_record(sqlite3_stmt* pStmt, int* arrData, int cData);
void insert_record(sqlite3_stmt* pStmt, int* arrData, int cData, bool* arrNull_p);
void update_idle_heartbeat();
