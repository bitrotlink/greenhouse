#include <wiringPi.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "gh_ctrl.h"

#define DEBUG_PRINT_ADC false
#define DEBUG_PRINT_FLUX false
#define DEBUG_PRINT_FLUX_CHANGE_ONLY true

//RPi BCM pins
#define SCLK 23
#define DOUT 24

// MAX11201B updates at 13.75 sps, but polling slower than that introduces occasional glitched data due to the chip unilaterally pulsing DOUT high to indicate a new conversion has been performed, which introduces a race condition between verifying DOUT is low and initiating a read, and the glitch can't be definitively filtered out since there's no CRC. And interrupts are impractical because wiringPi is crap.
// The solution is to poll faster than the update period, so the chip never gets a chance to send its unilateral pulse.
#define POLL_PERIOD (40*1000*1000) // ns
#define UPDATE_PERIOD ((double)1/13.75) // sec

#define ONE_SECOND (1000*1000*1000) // ns
#define RECALIBRATION_PERIOD 10 // Unit is calls to ADC_read
#define EMA_ALPHA 0.98 // For exponential moving average
#define SPIN_UP_DELAY ((double)5/(1.0 - EMA_ALPHA)) // Unit is calls to ADC_read
#define SPIN_UP_DELAY_SEC (SPIN_UP_DELAY*POLL_PERIOD/ONE_SECOND)
#define SPIN_UP_DELAY_SEC_LIMITED (SPIN_UP_DELAY*UPDATE_PERIOD) // Approximation, since polling will delay for another full POLL_PERIOD each time ADC isn't ready yet

#define SCLK_PERIOD 10000 // ns. 100kHz is adequate

#define REFP 3.3 // ADC positive reference voltage
#define FRAC_SCALE 8388608 // 2^23; ADC is 24-bit two's complement
#define MICROV_SCALE ((double)1000000*REFP/FRAC_SCALE)

char zSql_log[] = "insert into MAX11201B_logs values (?,?,?)";
const char* sensor_type = "MAX11201B";
double S_calib, ema_accum;
bool now_reading_p, flux_prev_inc_p;
int flux_mem;

void ADC_calibrate() {
  int in;
  struct timespec half_tick;
  half_tick.tv_sec = 0;
  half_tick.tv_nsec = SCLK_PERIOD/2;
  for(int i=0; i<26; i++) {
    digitalWrite(SCLK, 1);
    nanosleep(&half_tick, NULL);
    digitalWrite(SCLK, 0);
    nanosleep(&half_tick, NULL);
  }
  in=digitalRead(DOUT);
  if(!in) {
    fprintf(stderr, "ADC failure during calibration\n");
    exit(-1);
  }
}

int ADC_read() {
  int in;
  struct timespec half_tick;
  int val=0;
  half_tick.tv_sec = 0;
  half_tick.tv_nsec = SCLK_PERIOD/2;
  for(int i=0; i<24; i++) {
    int x;
    digitalWrite(SCLK, 1);
    nanosleep(&half_tick, NULL);
    digitalWrite(SCLK, 0);
    x=digitalRead(DOUT);
    if(x && (i==0)) val=-1; //First bit is sign bit
    else {
      val<<=1;
      val|=x;
    }
    nanosleep(&half_tick, NULL);
  }
  //25th clock per datasheet protocol to pull DOUT (i.e. RDY/DOUT) high until conversion ready
  digitalWrite(SCLK, 1);
  nanosleep(&half_tick, NULL);
  digitalWrite(SCLK, 0);
  nanosleep(&half_tick, NULL);
  in=digitalRead(DOUT);
  if(!in) {
    fprintf(stderr, "ADC failure during read\n");
    exit(-1);
  }
  ema_accum=(EMA_ALPHA*ema_accum)+(1.0 - EMA_ALPHA)*(double)val;
  return val;
  // And there's no CRC. Great job, Maxim... :-(
}

void output_val(int val) {
  if(DEBUG_PRINT_ADC) {
    double f_val = (double)val;
    double frac=(f_val)/FRAC_SCALE; //Range -1 to 1
    printf("0x%8X  %12.6f/1  %8.2fμV  %8.2fμV\n", val, frac, f_val*MICROV_SCALE, ema_accum*MICROV_SCALE);
  }
  double df_flux = ema_accum*MICROV_SCALE/S_calib;
  int i_flux = df_flux * HYST_SCALE;
  bool changed = reading_change(&flux_prev_inc_p, &flux_mem, i_flux);
  if(DEBUG_PRINT_FLUX) {
    if(DEBUG_PRINT_FLUX_CHANGE_ONLY) {
      if(changed) printf("\n%8.2f %d%s ", df_flux,
			 (flux_mem/HYST_SCALE),
			 (flux_prev_inc_p?"up":"dn"));
      else {
	printf(".");
	fflush(stdout);
      }
    } else printf("%8.2f %8.2f%s\n", df_flux,
		  ((double)flux_mem/HYST_SCALE),
		  changed?(flux_prev_inc_p?"up":"dn"):"");
  }
  if(changed) {
    int data = flux_mem/HYST_SCALE;
    insert_record(pStmt_log, &data, 1);
  } else update_idle_heartbeat();

}

void ready_interrupt() {
  int val;
  if(now_reading_p) return; //Ready line is same as data line (RDY/DOUT), so ignore interrupt while reading
  val=digitalRead(DOUT);
  if(val) return; //XXX: Spurious interrupt. Happens after every read; seems the wiringPi library delays the ISR call for a new interrupt until the previous call returns. Which also means the now_reading_p mutex is unnecessary.
  now_reading_p=true;
  val=ADC_read();
  output_val(val);
  now_reading_p=false;
}

//Using interrupts produces 100% CPU usage, even in test case with do-nothing ISR and only 14 interrupts per second. Crapware kernel or libraries.
//And at least wiringPi is crapware, apparently providing no way to unregister the ISR to avoid triggering when the MAX11201B RDY/DOUT pin is being used as DOUT (triggering is only needed when the pin is used as RDY).
void use_interrupts() {
  now_reading_p=false;
  struct timespec sleep;
  sleep.tv_sec = 0;
  sleep.tv_nsec = ONE_SECOND;
  if(wiringPiISR(DOUT, INT_EDGE_FALLING, &ready_interrupt)<0) {
    fprintf(stderr, "wiringPiISR setup failure: %s\n", strerror(errno));
    exit(-1);
  }
  while(1) {
    nanosleep(&sleep, NULL);
  }
}

void use_polling() {
  struct timespec sleep;
  sleep.tv_sec = 0;
  sleep.tv_nsec = POLL_PERIOD;
  int per_calib_count=0;
  int total_count=0;
  fprintf(stderr, "MAX11201B spin-up delay approx %ds...\n", (int)SPIN_UP_DELAY_SEC_LIMITED);
  while(1) {
    nanosleep(&sleep, NULL);
    int val;
    val=digitalRead(DOUT);
    if(val) continue; // ADC not ready yet
    val=ADC_read();
    per_calib_count++;
    total_count++;
    if(total_count>(int)SPIN_UP_DELAY) output_val(val);
    if(per_calib_count==RECALIBRATION_PERIOD) {
      per_calib_count=0;
      ADC_calibrate();
    }
  }
}

int main(int argc, char** argv) {
  int rc;
  sqlite3_stmt* pStmt_tmp;
  daemon_init(argc, argv);
  char zSql_select[]="select val from Config where var='PHFS_01e_S_calib'";
  rc = sqlite3_prepare_v3(db, zSql_select, -1, 0, &pStmt_tmp, NULL);
  check_sql(rc, "sqlite3_prepare failure");
  rc = sqlite3_step(pStmt_tmp);
  check_sql(rc, "sqlite3_step failure");
  if(rc!=SQLITE_ROW) {
    fprintf(stderr, "PHFS_01e_S_calib not found in Config table\n");
    exit(-1);
  }
  int dtype = sqlite3_column_type(pStmt_tmp, 0);
  check_sql(rc, "sqlite3_column_type failure");
  if(dtype!=2) {
    fprintf(stderr, "PHFS_01e_S_calib in Config table has wrong type: %s\n", Sqlite_type_names[dtype]);
    exit(-1);
  }
  S_calib = sqlite3_column_double(pStmt_tmp, 0);
  check_sql(rc, "sqlite3_column_double failure");
  sqlite3_finalize(pStmt_tmp);
  wiringPiSetupGpio();
  pinMode(SCLK, OUTPUT);
  pullUpDnControl(DOUT, PUD_UP);
  ADC_calibrate();
  ema_accum=0;
  //use_interrupts();
  use_polling();
  return 0;
}
