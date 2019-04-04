#include <wiringPiI2C.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include "gh_ctrl.h"

#define DEBUG_TIMING false
#define DEBUG_AC_CYCLE false
#define DEBUG_SAMPLES false
#define DEBUG_SPEED false
#define DEBUG_CURRENT_TRIGGER false
#define DEBUG_CURRENT_LIMIT 0.5
#define DEBUG_SUMMARY false
#define DEBUG_PRINT_RAW false

#define VP_MULTIPLIER 4.746 // To cancel the voltage divider that's used since the ina260 can only handle up to 40V (and read accurately up to 36V), but the circuit is measuring 120VAC, which is 170Vp. Divider top is 66kΩ, bottom is 18kΩ ∥ (Z_vbus=830kΩ). Within 1% of scope-measured 4.7826 (with 124Vrms, 176Vp, and 36.8V divider peak; Kill-a-watt read 122.7V, and multimeter read 120V).
#define DIODE_DROP 0.6 // To compensate the rectifier diode's voltage drop
#define INA260_I2C_ADDRESS 0x40
#define INA260_CONFIG_REG 0
#define INA260_V_REG 2
#define INA260_I_REG 1
#define INA260_P_REG 3

#define USE_TRUE_Vrms false // The ina260 internal Vmean is more stable than jittery samples read by the Pi to compute true Vrms

#define VIP_AVERAGING_MODE 0xdf68 // Mode is 0x68df, but wiringPi writes little endian and the chip reads big endian.
// Mode 0x68df configures the chip as follows:
#define VI_CONVERSION_TIME 588000 // ns
#define NUM_AVERAGES 128
// With these settings, reading both voltage and current, one averaging cycle = 0.588μS/sample * 2 samples/sequence * 128 sequences/cycle = 150.528ms. Multiplied by 60Hz gives 9.03168 AC power cycles per averaging cycle, which is only 0.352% more than an integer number of power cycles, which maximizes accuracy.

#define V_MODE 0xde60 // Mode is 0x60de. Configures for 588μs conversion time and no averaging.
#define I_MODE 0xdd60 // Mode is 0x60dd. Configures for 588μs conversion time and no averaging.
#define N_CONVERSION_TIME 588000 // ns

#define AC_PERIOD 16666667 // ns (60Hz)
#define N_CYCLES 2 // Quantity of 60Hz power cycles to read per V or I RMS measurement
#define N_SAMPLES AC_PERIOD*N_CYCLES*3/2/N_CONVERSION_TIME+1 // Need 3/2 of a power cycle to ensure two zero-positive crossings are sampled, since sampling will begin at an unknown position of the cycle.

char zSql_log[] = "insert into INA260_logs values (?,?,?,?,?)";
const char* sensor_type = "INA260";
int fd_ina260, Vmean_raw, Pmean_raw;
double Vmean_raw_V, Pmean_raw_W, Vmean, Pmean, Nrms, Vrms, Irms, S, PF;
bool Vrms_success, Irms_success;

int n_a[N_SAMPLES];
struct timespec read_times[N_SAMPLES];
struct timespec remaining[N_SAMPLES];

bool Vrms_prev_inc_p, Irms_prev_inc_p, Pmean_prev_inc_p;
int Vrms_mem, Irms_mem, Pmean_mem;

struct timespec ts_conv_period, // How long a single-shot conversion takes. Constant.
  ts_avg_conv_period, // How long an averaging cycle takes. Constant.
  ts_avg_conv_start; // For sleeping until averaging cycle is done.

int read_reg(int fd, int reg) { // 16 bit, signed, big endian from chip
  int val = wiringPiI2CReadReg16(fd, reg);
  int upper = (val&0x00ff)<<8;
  int sign = upper & 0x8000;
  if(sign) upper|=(-1>>16)<<16;
  int lower = (val&0xff00)>>8;
  return upper+lower;
}

// Return first zero positive crossing at or following the start offset, or return len if none found
int find_zero_positive_crossing(int* array, int len, int start) {
  for(int x=start; x<len-1; x++) {
    if((array[x] <= 0) && (array[x+1] > 0)) return x+1;
  }
  return len;
}

bool ts_positive_p(struct timespec* ts) {
  if((ts->tv_sec>0) ||
     ((ts->tv_sec==0) && (ts->tv_nsec>0)))
    return true;
  return false;
}

void get_averages() {
  struct timespec ts, ts_avg_conv_elapsed;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts_diff(&ts_avg_conv_elapsed, &ts, &ts_avg_conv_start);
  ts_diff(&ts, &ts_avg_conv_period, &ts_avg_conv_elapsed); // How much longer to wait for averaging cycle to complete
  if(DEBUG_TIMING) printf("Averaging cycle elapsed: %ld:%ld ;  Will sleep: %ld:%ld\n", ts_avg_conv_elapsed.tv_sec, ts_avg_conv_elapsed.tv_nsec, ts.tv_sec, ts.tv_nsec);
  nanosleep(&ts, NULL);
  Vmean_raw = read_reg(fd_ina260, INA260_V_REG);
  // int Imean_raw = read_reg(fd_ina260, 1); // Current not read here, since the chip outputs average of samples instead of average of absolute values of samples, so for AC the average is a useless near-zero result
  Pmean_raw = read_reg(fd_ina260, INA260_P_REG);
  clock_gettime(CLOCK_MONOTONIC, &ts_avg_conv_start); // Avoid reading registers again until next averaging cycle is complete
  Vmean_raw_V = (double)(Vmean_raw)*1.25/1000; // Chip output unit is 1.25 mV.
  Vmean = Vmean_raw_V*VP_MULTIPLIER*2; // The *2 is because the circuit uses a half-wave diode rectifier (since the ina260 can't handle negative voltages), so half the voltage samples are zero (since the circuit is reading AC), so the average output by the chip is half the true average (of absolute values).
  Pmean_raw_W = (double)Pmean_raw*10/1000; // Chip output unit is 10 mW.
  Pmean = Pmean_raw_W*VP_MULTIPLIER*2; // Chip output is average of V*I samples, so the output is useful even for AC. But since half the voltage sine wave is missing due to the half-wave rectifier, the measured power is only 1/2 of the true power.
}

bool get_n(int mode, int reg, double multiplier, double additive) { // Get Vrms or Irms
  struct timespec ts, ts_start, ts_end, ts_elapsed;
  wiringPiI2CWriteReg16(fd_ina260, INA260_CONFIG_REG, mode);
  ts.tv_sec = 0;
  ts.tv_nsec = N_CONVERSION_TIME;
  nanosleep(&ts, NULL); // Give time for first conversion after the config change
  for(int i=0; i<N_SAMPLES; i++) {
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    n_a[i] = read_reg(fd_ina260, reg);
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    ts_diff(&ts_elapsed, &ts_end, &ts_start); // How long read_reg took
    ts_diff(&ts, &ts_conv_period, &ts_elapsed); // How much longer to wait for next conversion
    if(DEBUG_TIMING) read_times[i] = ts_elapsed;
    if(DEBUG_TIMING) remaining[i] = ts;
    if(ts_positive_p(&ts)) nanosleep(&ts, NULL);
  }
  wiringPiI2CWriteReg16(fd_ina260, INA260_CONFIG_REG, VIP_AVERAGING_MODE); // Restore it now, since one averaging cycle takes a long time (over 150ms).
  clock_gettime(CLOCK_MONOTONIC, &ts_avg_conv_start);
  int AC_period_start = find_zero_positive_crossing(n_a, N_SAMPLES, 0);
  int AC_period_end = AC_period_start;
  for(int i=0; i<N_CYCLES; i++)
    AC_period_end = find_zero_positive_crossing(n_a, N_SAMPLES, AC_period_end);
  Nrms = 0;
  for(int i=AC_period_start; i<AC_period_end; i++) {
    double N = (double)(n_a[i])*1.25/1000*multiplier + additive; // Chip output unit is 1.25 mV or mA.
    Nrms += N*N;
  }
  bool Nrms_success = false;
  if((AC_period_start==AC_period_end) ||
     (AC_period_end == N_SAMPLES)) { // Failed to find the end of a full cycle
    Nrms=0;
  }
  else {
    Nrms /= (AC_period_end - AC_period_start);
    Nrms = sqrt(Nrms);
    Nrms_success = true;
  }
  if(DEBUG_SAMPLES) {
    printf("Register %d:\n", reg);
    for(int i=0; i<N_SAMPLES; i++) {
      double N = (double)(n_a[i])*1.25/1000;
      if(DEBUG_TIMING) printf("[timing%ld:%ld %ld:%ld] ", read_times[i].tv_sec, read_times[i].tv_nsec, remaining[i].tv_sec, remaining[i].tv_nsec);
      printf(reg==INA260_V_REG?"%fV":"%fA", N);
      if(DEBUG_AC_CYCLE) {
	if((AC_period_start<=i) && (i<AC_period_end)) printf(" ***");
	if(AC_period_start==i) printf(" START");
	if(AC_period_end==i) printf(" END");
      }
      printf("\n");
    }
    printf("\n");
  }
  return Nrms_success;
}

void setup() {
  ts_conv_period.tv_sec = 0;
  ts_conv_period.tv_nsec = N_CONVERSION_TIME;
  ts_avg_conv_period.tv_sec = 0;
  ts_avg_conv_period.tv_nsec = VI_CONVERSION_TIME * NUM_AVERAGES * 2; // *2 because VI_CONVERSION_TIME is for each of voltage and current (measured sequentially, since the chip has only one ADC)

  fd_ina260 = wiringPiI2CSetup(INA260_I2C_ADDRESS) ;
  if(fd_ina260==-1) exit(errno);
  wiringPiI2CWriteReg16(fd_ina260, INA260_CONFIG_REG, VIP_AVERAGING_MODE);
  clock_gettime(CLOCK_MONOTONIC, &ts_avg_conv_start);
}


void read_all() {
  struct timespec ts_all_start, ts_all_end, ts_all_elapsed;
  if(DEBUG_SPEED) clock_gettime(CLOCK_MONOTONIC, &ts_all_start);
  get_averages();
  if(USE_TRUE_Vrms) {
    Vrms_success = get_n(V_MODE, INA260_V_REG, VP_MULTIPLIER, DIODE_DROP);
    Vrms = Nrms*sqrt(2);
  }
  else {
    Vrms_success = true;
    Vrms = Vmean * 1.11 + DIODE_DROP; // Assuming it's a sine wave, which line power (nominal 120VAC) is, except it's half-wave rectified in the circuit.
  }
  Irms_success = get_n(I_MODE, INA260_I_REG, 1, 0); // Full current is directly through the ina260's shunt resistor, so unlike the case with voltage (and thus power), no scaling is needed to compensate for the circuit.
  Irms = Nrms;
  if(Vrms_success && Irms_success) {
    S = Vrms * Irms;
    Pmean += Pmean*DIODE_DROP/Vrms;
    (S==0)?(PF=1):(PF = Pmean / S);
  } else fprintf(stderr, "Vrms_success=%d Irms_success=%d; skipping since not both true\n", Vrms_success, Irms_success);

  if(DEBUG_SPEED) {
    clock_gettime(CLOCK_MONOTONIC, &ts_all_end);
    ts_diff(&ts_all_elapsed, &ts_all_end, &ts_all_start);
  }

  if(DEBUG_CURRENT_TRIGGER) {
    if(Irms<DEBUG_CURRENT_LIMIT) {
      printf(".");
      fflush(stdout);
      return;
    } else {
      printf("\n");
    }
  }

  int i_Vrms = Vrms * HYST_SCALE;
  int i_Irms = Irms * HYST_SCALE *100;
  int i_Pmean = Pmean * HYST_SCALE;
  bool Vrms_changed = reading_change(&Vrms_prev_inc_p, &Vrms_mem, i_Vrms);
  bool Irms_changed = reading_change(&Irms_prev_inc_p, &Irms_mem, i_Irms);
  bool Pmean_changed = reading_change(&Pmean_prev_inc_p, &Pmean_mem, i_Pmean);
  int arrData[3];
  arrData[0] = Vrms_mem/HYST_SCALE;
  arrData[1] = Irms_mem/HYST_SCALE;
  arrData[2] = Pmean_mem/HYST_SCALE;

  if(DEBUG_SUMMARY) {
    if(DEBUG_PRINT_RAW) {
      printf("%fVmean %fW\n", Vmean, (double)(Pmean));
      if(Vrms_success && Irms_success) {
	if(USE_TRUE_Vrms)
	  printf("Vrms/Vmean=%f\n", Vrms/Vmean);
	printf("%fVrms %fArms %fVA  PF=%f\n", Vrms, Irms, S, PF);
	printf("%fVmean_raw_V %fPmean_raw_W\n", Vmean_raw_V, Pmean_raw_W);
	if(DEBUG_SPEED) printf("Total time: %fms\n", ts_all_elapsed.tv_sec+(double)ts_all_elapsed.tv_nsec/1000000);
	printf("********\n\n");
      }
      else printf("Failed to find Vrms and Irms\n");
    } else printf("%d Vrms %d Irms %d Pmean %d\n", arrData[0], arrData[1], arrData[2], (Vrms_changed || Irms_changed || Pmean_changed));
  }
  if((Vrms_success && Irms_success)
     &&(Vrms_changed || Irms_changed || Pmean_changed))
    insert_record(pStmt_log, arrData, 3);
  else update_idle_heartbeat();
}

int main(int argc, char** argv) {
  int rc;
  setup();
  daemon_init(argc, argv);
  while(true) {
    read_all();
  }
  return 0;
}
