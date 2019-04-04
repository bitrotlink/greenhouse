#include <wiringPi.h>
#include <stdio.h>
#include <time.h>
#include "gh_ctrl.h"

#define DEBUG_PRINT false

#define LOGGING_PERIOD 100000000 // ns (i.e. 100ms)

//RPi BCM pins
#define FURNACE_SENSE_Q1 5
#define FURNACE_SENSE_Q2 6

// Transistors are numbered from 1; array elements are numbered from zero. I already got one bug because of this; let's prevent recurrence. Array q is in the loop in main() below.
#define Q1 (q[0])
#define Q2 (q[1])

char zSql_log[] = "insert into Furnace_logs values (?,?,?,?)";
const char* sensor_type = "Furnace";
int q1_prev, q2_prev;

int main(int argc, char** argv) {
  int rc;
  daemon_init(argc, argv);
  wiringPiSetupGpio();
  pullUpDnControl(FURNACE_SENSE_Q1, PUD_UP);
  pullUpDnControl(FURNACE_SENSE_Q2, PUD_UP);
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = LOGGING_PERIOD;

  while(1) {
    int q[2];
    Q1=digitalRead(FURNACE_SENSE_Q1);
    Q2=digitalRead(FURNACE_SENSE_Q2);
    if(DEBUG_PRINT) {
      printf("%d %d furnace is %s\n", Q1, Q2, ((Q1==0)&&(Q2==0))?"on":"off");
      fflush(stdout);
    }
    if((q1_prev!=Q1) || (q2_prev!=Q2)) {
      q1_prev = Q1;
      q2_prev = Q2;
      insert_record(pStmt_log, q, 2);
    } else update_idle_heartbeat();
    nanosleep(&ts, NULL);
  }
  return 0;
}
