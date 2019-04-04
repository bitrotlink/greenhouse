#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <execinfo.h>
#include "gh_ctrl.h"

#define IDLE_HEARTBEAT_LOGGING_PERIOD 5 // seconds
#define BT_BUF_SIZE 100
#ifndef DEBUG_PRINT_BACKTRACE
#define DEBUG_PRINT_BACKTRACE false
#endif

sqlite3* db;
sqlite3_stmt* pStmt_log;
sqlite3_stmt* pStmt_hb;
const char* Sqlite_type_names[] = {
  "",
  "INT",
  "FLOAT",
  "TEXT",
  "BLOB",
  "NULL"
};

const char zSql_hb[] = "INSERT INTO Idle_heartbeats VALUES (?, ?) ON CONFLICT(sensor_name) DO UPDATE SET sec=excluded.sec";

struct timespec ts_heartbeat; // Time of last record insertion, or last idle heartbeat, whichever is later.

// timespec diff. Result value is tv_sec + tv_nsec, as usual. Result can be negative, and tv_sec is integer floor of the real value; tv_nsec is thus always positive.
void ts_diff(struct timespec* result, struct timespec* a, struct timespec* b) {
  result->tv_sec = a->tv_sec - b->tv_sec;
  result->tv_nsec = a->tv_nsec - b->tv_nsec;
  if(result->tv_nsec<0) {
    result->tv_nsec += 1000000000;
    result->tv_sec -= 1;
  }
}

// Hysteretically resist transitions between increasing and decreasing, to prevent jitter. If the movement direction is the same as previous, or it reverses and the hysteresis is overcome, record the new reading and return true.
bool reading_change(bool* px_prev_inc_p, int* px_mem, int x) {
  int x_mem = *px_mem;
  if(((*px_prev_inc_p) && (x>(x_mem)))
     || (!(*px_prev_inc_p) && (x<x_mem))) {
    // New movement direction is same as previous
    *px_mem = x;
    return (x_mem/HYST_SCALE)!=(x/HYST_SCALE);
  } else if((*px_prev_inc_p) && x<(x_mem - 1)) {
    // Transition from increasing to decreasing
    *px_mem = x;
    *px_prev_inc_p = false;
    return (x_mem/HYST_SCALE)!=(x/HYST_SCALE);
  } else if(!(*px_prev_inc_p) && x>(x_mem + 1)) {
    // Transition from decreasing to increasing
    *px_mem = x;
    *px_prev_inc_p = true;
    return (x_mem/HYST_SCALE)!=(x/HYST_SCALE);
  }
  // Either no movement, or insufficient to overcome hysteresis
  return false;
}

void check_sql(int rc, const char* msg) {
  if(rc!=SQLITE_OK && rc!=SQLITE_ROW && rc!=SQLITE_DONE) {
    fprintf(stderr, "%s", msg);
    fprintf(stderr, ": %s (%d)\n", sqlite3_errstr(rc), rc);
    fprintf(stderr, "%s\n", sqlite3_errmsg(db)); // Because Sqlite strangely omits the details from rc despite sqlite3_extended_result_codes being enabled. E.g. when sqlite3_prepare_v3 is called to prepare "CREATE TABLE foo(x INT)" when foo already exists, rc is just the entirely useless generic SQLITE_ERROR.
    sqlite3_close(db);
    exit(rc);
  }
}

void print_backtrace() {
  void* buffer[BT_BUF_SIZE];
  int nptrs = backtrace(buffer, BT_BUF_SIZE);
  fprintf(stderr, "Backtrace depth %d:\n", nptrs);
  fflush(stderr);
  backtrace_symbols_fd(buffer, nptrs, fileno(stderr));
  fprintf(stderr, "End of backtrace.\n");
}

int ghpi_sqlite_busy_handler(void* argv0, int count) {
  const char* prog_name = argv0?(char*)argv0:"";
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = GHPI_SQL_BUSY_WAIT;
  if(count>=GHPI_SQL_BUSY_RETRY_MAX) {
    fprintf(stderr, "%s: DB busy for max %d retries; aborting\n", prog_name, GHPI_SQL_BUSY_RETRY_MAX);
    if(DEBUG_PRINT_BACKTRACE) print_backtrace();
    return 0;
  }
  if(count>=GHPI_SQL_BUSY_NOTICE_THRESHOLD) {
    fprintf(stderr, "%s: DB busy; sleeping %dms after try #%d\n", prog_name, GHPI_SQL_BUSY_WAIT/1000000, count);
    if(DEBUG_PRINT_BACKTRACE) print_backtrace();
  }
  nanosleep(&ts, NULL);
  return 1;
}

void ghpi_sqlite_init(const char* argv0, const char* db_file_name) {
  int rc = sqlite3_open_v2(db_file_name, &db, SQLITE_OPEN_READWRITE /* | SQLITE_OPEN_CREATE */, NULL);
  check_sql(rc, "sqlite3_open failure");
  sqlite3_extended_result_codes(db, 1);
  // rc = sqlite3_busy_timeout(db, 5000);
  rc = sqlite3_busy_handler(db, &ghpi_sqlite_busy_handler, (void*)argv0);
  check_sql(rc, "sqlite3_busy_handler failure");
  char* zErrMsg = 0;
  rc = sqlite3_exec(db, GHPI_SQLITE_INIT_STRING, NULL, NULL, &zErrMsg);
  if(rc!=SQLITE_OK) {
    fprintf(stderr, "sqlite3_exec error in ghpi_sqlite_init: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
    sqlite3_close(db);
    exit(rc);
  }
  char zSql_select_last_log[]="select sec from Last_log_time";
  sqlite3_stmt* pStmt_tmp;
  rc = sqlite3_prepare_v3(db, zSql_select_last_log, -1, 0, &pStmt_tmp, NULL);
  check_sql(rc, "sqlite3_prepare failure in ghpi_sqlite_init");
  rc = sqlite3_step(pStmt_tmp);
  check_sql(rc, "sqlite3_step failure in ghpi_sqlite_init");
  if(rc!=SQLITE_ROW) { // Even for empty DB, Last_log_sec is 0, not omitted
    fprintf(stderr, "Error getting last log timestamp in ghpi_sqlite_init\n");
    exit(-1);
  }
  int last_log_sec = sqlite3_column_int(pStmt_tmp, 0);
  int last_log_cs = sqlite3_column_int(pStmt_tmp, 1);
  sqlite3_finalize(pStmt_tmp);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts_heartbeat);
  clock_gettime(CLOCK_REALTIME, &ts);
  if((ts.tv_sec < last_log_sec)
     || ((ts.tv_sec == last_log_sec)
	 && ((ts.tv_nsec >> TS_TV_NSEC_SHIFT) < last_log_cs))) {
    fprintf(stderr, "Current time is %ld seconds (rounded down) behind last logged time. Aborting.\n", last_log_sec - ts.tv_sec);
    exit(-1);
  }
  rc = sqlite3_prepare_v3(db, zSql_log, -1, SQLITE_PREPARE_PERSISTENT, &pStmt_log, NULL);
  check_sql(rc, "sqlite3_prepare failure");
  rc = sqlite3_prepare_v3(db, zSql_hb, -1, SQLITE_PREPARE_PERSISTENT, &pStmt_hb, NULL);
  check_sql(rc, "sqlite3_prepare failure");
}

void daemon_init(int argc, char** argv) {
  if(argc!=2) {
    fprintf(stderr, "Usage: %s db-file\n", argv[0]);
    exit(-1);
  }
  time_t time_start = time(NULL);
  char ts_buf[64];
  struct tm* tm_info;
  tm_info = localtime(&time_start);
  strftime(ts_buf, 64, "%Y-%m-%d %H:%M:%S", tm_info);
  fprintf(stderr, "%s Starting %s%s\n", ts_buf, argv[0],
	  DEBUG_PRINT_BACKTRACE?" with backtracing enabled":"");
  ghpi_sqlite_init(argv[0], argv[1]);
}

// If arrNull_p is non-null, then it must point to an array of length cData of bools. For each true bool, a null is inserted, and the corresponding value of arrData is ignored.
void insert_record(sqlite3_stmt* pStmt, int* arrData, int cData, bool* arrNull_p) {
  int rc, i;
  for(i=0; i<cData; i++) {
    // parameters are numbered from 1, and first two are the timestamp (sec and cs), not sensor reading data
    if(arrNull_p && (arrNull_p[i]==true)) rc = sqlite3_bind_null(pStmt, i+3);
      else rc = sqlite3_bind_int(pStmt, i+3, arrData[i]);
    check_sql(rc, "sqlite3_bind_null or sqlite3_bind_int failure in insert_record while binding data");
  }
  struct timespec rt;
  clock_gettime(CLOCK_REALTIME, &rt);
  for(i=0; i<TIMESTAMP_RETRY_MAX; i++) {
    rc = sqlite3_bind_int(pStmt, 1, rt.tv_sec);
    check_sql(rc, "sqlite3_bind_int failure in insert_record while binding rt.tv_sec");
    rc = sqlite3_bind_int(pStmt, 2, (rt.tv_nsec >> TS_TV_NSEC_SHIFT));
    check_sql(rc, "sqlite3_bind_int failure in insert_record while binding rt.tv_nsec");
    rc = sqlite3_step(pStmt);
    if(rc==SQLITE_CONSTRAINT_UNIQUE) { // Timestamp collision
      rt.tv_nsec += 1 << TS_TV_NSEC_SHIFT; // Increment by approximately one centisecond
      if(rt.tv_nsec >= 1000000000) {
	rt.tv_nsec -= 1000000000;
	rt.tv_sec += 1;
      }
      sqlite3_reset(pStmt);
      char ts_buf[64];
      struct tm* tm_info;
      tm_info = localtime(&(rt.tv_sec));
      strftime(ts_buf, 64, "%Y-%m-%d %H:%M:%S", tm_info);
      fprintf(stderr, "%s Timestamp collision during attempt %d to insert log record. Retrying.\n", ts_buf, i);
      continue;
    }
    check_sql(rc, "sqlite3_step failure in insert_record");
    sqlite3_reset(pStmt);
    break;
  }
  if(i==TIMESTAMP_RETRY_MAX) {
    char ts_buf[64];
    struct tm* tm_info;
    tm_info = localtime(&(rt.tv_sec));
    strftime(ts_buf, 64, "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(stderr, "%s Failed to insert record due to timestamp collisions in excess of %d. Dropping it.\n", ts_buf, TIMESTAMP_RETRY_MAX);
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &ts_heartbeat);
}

// Shorter signature for when no nulls need to be inserted
void insert_record(sqlite3_stmt* pStmt, int* arrData, int cData) {
  insert_record(pStmt, arrData, cData, NULL);
}

void update_idle_heartbeat() {
  struct timespec mono, diff;
  clock_gettime(CLOCK_MONOTONIC, &mono);
  ts_diff(&diff, &mono, &ts_heartbeat);
  ts_heartbeat = mono;
  if(diff.tv_sec >= IDLE_HEARTBEAT_LOGGING_PERIOD) {
    int rc = sqlite3_bind_text(pStmt_hb, 1, sensor_type, -1, SQLITE_STATIC);
    check_sql(rc, "sqlite3_bind_text failure in update_idle_heartbeat");
    rc = sqlite3_bind_int(pStmt_hb, 2, diff.tv_sec);
    check_sql(rc, "sqlite3_bind_int failure in update_idle_heartbeat");
    rc = sqlite3_step(pStmt_hb);
    check_sql(rc, "sqlite3_step failure in update_idle_heartbeat");
    sqlite3_reset(pStmt_hb);
  }
}
