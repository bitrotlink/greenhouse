#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "gh_ctrl.h"

#define LOOP_PERIOD 250*1000000 // ns
#define N_DS18B20_MAX 64 // 64 DS18B20 sensors ought to be enough for anybody...
#define STATBUFLEN 1024

bool have_wireless;
FILE* proc_wireless_fp;
const char* statfile = "/proc/net/wireless";
char statbuf[STATBUFLEN];

int ncols;
int DS18B20_IDs[N_DS18B20_MAX];
int DS18B20_readings[N_DS18B20_MAX];
sqlite3_stmt* pStmt_logs;
sqlite3_stmt* pStmt_DS18B20_IDs;
sqlite3_stmt* pStmt_DS18B20_logs;

void read_print_logs() { // Get the single-row result from the view, and print in CSV format, with header separated by "!" instead of "\n" (for easy transmission as one message via websocketd, which splits messages by "\n")
  int rc, i;
  rc = sqlite3_step(pStmt_logs);
  check_sql(rc, "sqlite3_step failure in poll_stream/read_print_logs");
  if(rc!=SQLITE_ROW) {
    sqlite3_reset(pStmt_logs);
    return;
  }
  for(i=0; i<ncols; i++) {
    printf("%s%s", sqlite3_column_name(pStmt_logs, i),
	   (i==(ncols-1))?"":",");
  }
  printf("!");
  for(i=0; i<ncols; i++) {
    if(sqlite3_column_type(pStmt_logs, i)==SQLITE_NULL)
      printf("null%s", (i==(ncols-1))?"":",");
    else printf("%d%s", sqlite3_column_int(pStmt_logs, i),
		(i==(ncols-1))?"":",");
  }
  printf("\n");
  sqlite3_reset(pStmt_logs);
}

void read_print_DS18B20_logs() {
  int rc, i, n_DS18B20=0;
  while(1) { // Print all the labels, and get all the readings
    rc = sqlite3_step(pStmt_DS18B20_IDs);
    check_sql(rc, "sqlite3_step failure in poll_stream/read_print_DS18B20_logs");
    if(rc==SQLITE_ROW) {
      if(n_DS18B20!=0) printf(","); // Delimit previous output of this loop
      DS18B20_IDs[n_DS18B20++] = sqlite3_column_int(pStmt_DS18B20_IDs, 0);
      printf("%s", sqlite3_column_text(pStmt_DS18B20_IDs, 1));
    } else {
      sqlite3_reset(pStmt_DS18B20_IDs);
      break;
    }
  }
  printf("!");
  for(i=0; i<n_DS18B20; i++) { // Print all the readings
    rc = sqlite3_bind_int(pStmt_DS18B20_logs, 1, DS18B20_IDs[i]);
    check_sql(rc, "sqlite3_bind_int failure in read_print_DS18B20_logs");
    rc = sqlite3_step(pStmt_DS18B20_logs);
    check_sql(rc, "sqlite3_step failure in read_print_DS18B20_logs");
    if(rc==SQLITE_DONE) printf("n/a%s", (i==(n_DS18B20-1))?"":","); // No data
    else if(sqlite3_column_type(pStmt_DS18B20_logs, 0)==SQLITE_NULL)
      printf("null%s", (i==(n_DS18B20-1))?"":",");
    else printf("%d%s", sqlite3_column_int(pStmt_DS18B20_logs, 0),
		(i==(n_DS18B20-1))?"":",");
    sqlite3_reset(pStmt_DS18B20_logs);
  }
  printf("\n");
}

void read_print_wlan_stats() {
  int retval = fseek(proc_wireless_fp, 0, SEEK_SET);
  if(retval) {
    printf("wlan0_level!err\n");
    return;
  }
  size_t len = fread(statbuf, sizeof(char), STATBUFLEN, proc_wireless_fp);
  if(ferror(proc_wireless_fp)!=0) {
    printf("wlan0_level!err\n");
    return;
  }
  if(len>=STATBUFLEN) { // Data too big for static buffer
    printf("wlan0_level!err\n");
    return;
  }
  statbuf[len]=(char)0;
  char* wlan0 = strstr(statbuf, "wlan0");
  if(wlan0==NULL) {
    printf("wlan0_level!null\n");
    return;
  }
  char* state;
  char* val = strtok_r(wlan0, " .", &state); // iface
  val = strtok_r(NULL, " .", &state); // status
  val = strtok_r(NULL, " .", &state); // link quality
  val = strtok_r(NULL, " .", &state); // signal level
  printf("wlan0_level!%s\n", val);
}

void setup() {
  int rc;
  char zSql_get_logs[] = "SELECT * FROM Last_logs";
  char zSql_get_DS18B20_IDs[] = "SELECT sensor_ID, label from DS18B20_IDs";
  char zSql_get_DS18B20_logs[] =
    "SELECT temp from DS18B20_logs WHERE sensor_ID=? AND sec>(strftime('%s', 'now')-3600) ORDER BY sec DESC, cs DESC LIMIT 1";
  rc = sqlite3_prepare_v3(db, zSql_get_logs, -1,
			  SQLITE_PREPARE_PERSISTENT, &pStmt_logs, NULL);
  check_sql(rc, "sqlite3_prepare failure in poll_stream for logs");
  ncols = sqlite3_column_count(pStmt_logs);
  rc = sqlite3_prepare_v3(db, zSql_get_DS18B20_IDs, -1,
			  SQLITE_PREPARE_PERSISTENT, &pStmt_DS18B20_IDs, NULL);
  check_sql(rc, "sqlite3_prepare failure in poll_stream for DS18B20_IDs");
  rc = sqlite3_prepare_v3(db, zSql_get_DS18B20_logs, -1,
			  SQLITE_PREPARE_PERSISTENT, &pStmt_DS18B20_logs, NULL);
  check_sql(rc, "sqlite3_prepare failure in poll_stream for DS18B20_logs");
  proc_wireless_fp = fopen(statfile, "r");
  if(proc_wireless_fp==NULL) have_wireless = false;
  else have_wireless = true;
}

int main(int argc, char** argv) {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = LOOP_PERIOD;
  daemon_init(argc, argv);
  setup();
  while(1) {
    read_print_logs();
    read_print_DS18B20_logs();
    read_print_wlan_stats();
    fflush(stdout);
    nanosleep(&ts, NULL);
  }
  return 0;
}
