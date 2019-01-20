#include <wiringPiI2C.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>

#define DEBUG_TIMING false
#define DEBUG_AC_CYCLE false
#define DEBUG_SAMPLES false
#define DEBUG_SUMMARY true

#define VP_MULTIPLIER 4.7826 // To cancel the voltage divider that's used since the ina260 can only handle up to 40V (and read accurately up to 36V), but the circuit is measuring 120VAC, which is 170Vp.
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

int fd_ina260, Vmean_raw, Pmean_raw;
double Vmean, Pmean, Nrms, Vrms, Irms, S, PF;
bool Vrms_success, Irms_success;

int n_a[N_SAMPLES];
struct timespec read_times[N_SAMPLES];
struct timespec remaining[N_SAMPLES];

struct timespec ts;
struct timespec ts_start;
struct timespec ts_end;
struct timespec ts_elapsed;
struct timespec ts_conv;

int read_reg(int fd, int reg) { //16 bit, signed, big endian from chip
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

// timespec diff. Result value is tv_sec + tv_nsec, as usual. Result can be negative, and tv_sec is integer floor of the real value; tv_nsec is thus always positive.
void ts_diff(struct timespec* result, struct timespec* a, struct timespec* b) {
  result->tv_sec = a->tv_sec - b->tv_sec;
  result->tv_nsec = a->tv_nsec - b->tv_nsec;
  if(result->tv_nsec<0) {
    result->tv_nsec += 1000000000;
    result->tv_sec -= 1;
  }
}

bool ts_positive_p(struct timespec* ts) {
  if((ts->tv_sec>0) ||
     ((ts->tv_sec==0) && (ts->tv_nsec>0)))
    return true;
  return false;
}

void get_averages() {
  wiringPiI2CWriteReg16(fd_ina260, INA260_CONFIG_REG, VIP_AVERAGING_MODE);
  ts.tv_sec = 0;
  ts.tv_nsec = VI_CONVERSION_TIME * NUM_AVERAGES * 2; // *2 because VI_CONVERSION_TIME is for each of voltage and current (measured sequentially, since the chip has only one ADC)
  nanosleep(&ts, NULL); // Give time for the first cycle of conversions and averaging after the config change
  Vmean_raw = read_reg(fd_ina260, INA260_V_REG);
  // int Imean_raw = read_reg(fd_ina260, 1); // Current not read here, since the chip outputs average of samples instead of average of absolute values of samples, so for AC the average is a useless near-zero result
  Pmean_raw = read_reg(fd_ina260, INA260_P_REG);
  Vmean = (double)(Vmean_raw)*1.25/1000*VP_MULTIPLIER*2; // Chip output unit is 1/1.25 mV. The *2 is because the circuit uses a half-wave diode rectifier (since the ina260 can't handle negative voltages), so half the voltage samples are zero (since the circuit is reading AC), so the average output by the chip is half the true average (of absolute values)
  Pmean = Pmean_raw*10/1000*VP_MULTIPLIER*sqrt(2); // Chip output unit is 1/10 mW. Chip output fortunately is average of absolute values of V*I, so the output is useful even for AC. But since half the voltage sine wave is missing due to the half-wave rectifier, the measured power is only 1/√2 of the true power, thus the *√2 here.
}

bool get_n(int mode, int reg, double multiplier) {
  wiringPiI2CWriteReg16(fd_ina260, INA260_CONFIG_REG, mode);
  ts.tv_sec = 0;
  ts.tv_nsec = N_CONVERSION_TIME;
  nanosleep(&ts, NULL); // Give time for first conversion after the config change
  for(int i=0; i<N_SAMPLES; i++) {
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    n_a[i] = read_reg(fd_ina260, reg);
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    ts_diff(&ts_elapsed, &ts_end, &ts_start);
    ts_diff(&ts, &ts_conv, &ts_elapsed);
    if(DEBUG_TIMING) read_times[i] = ts_elapsed;
    if(DEBUG_TIMING) remaining[i] = ts;
    if(ts_positive_p(&ts)) nanosleep(&ts, NULL);
  }
  wiringPiI2CWriteReg16(fd_ina260, INA260_CONFIG_REG, VIP_AVERAGING_MODE); // Restore it now, since one averaging cycle takes a long time (over 150ms).
  int AC_period_start = find_zero_positive_crossing(n_a, N_SAMPLES, 0);
  int AC_period_end = AC_period_start;
  for(int i=0; i<N_CYCLES; i++)
    AC_period_end = find_zero_positive_crossing(n_a, N_SAMPLES, AC_period_end);
  Nrms = 0;
  for(int i=AC_period_start; i<AC_period_end; i++) {
    double N = (double)(n_a[i])*1.25/1000*multiplier; // Chip output unit is 1/1.25 mV or mA.
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
      if(DEBUG_TIMING) printf("%ld:%ld %ld:%ld", read_times[i].tv_sec, read_times[i].tv_nsec, remaining[i].tv_sec, remaining[i].tv_nsec);
      printf("%fA", N);
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

int main (void)
{
  ts_conv.tv_sec = 0;
  ts_conv.tv_nsec = N_CONVERSION_TIME;

  fd_ina260 = wiringPiI2CSetup (INA260_I2C_ADDRESS) ;
  if(fd_ina260==-1) return errno;
  get_averages();
  if(USE_TRUE_Vrms) {
    Vrms_success = get_n(V_MODE, INA260_V_REG, VP_MULTIPLIER*sqrt(2));
    Vrms = Nrms;
  }
  else {
    Vrms_success = true;
    Vrms = Vmean * 1.11; // Assuming it's a sine wave, which line power (nominal 120VAC) is, except it's half-wave rectified in the circuit.
  }
  Irms_success = get_n(I_MODE, INA260_I_REG, 1); // Full current is directly through the ina260's shunt resistor, so unlike the case with voltage (and thus power), no scaling is needed to compensate for the circuit.
  Irms = Nrms;
  if(Vrms_success && Irms_success) {
    S = Vrms * Irms;
    PF = Pmean / S;
  }

  if(DEBUG_SUMMARY) {
    printf("%fVmean %fW\n", Vmean, (double)(Pmean));
    if(Vrms_success && Irms_success) {
      if(USE_TRUE_Vrms)
	printf("Vrms/Vmean=%f\n", Vrms/Vmean);
      printf("%fVrms %fArms %fVA  PF=%f\n********\n\n", Vrms, Irms, S, PF);
    }
    else printf("Failed to find Vrms and Irms\n");
  }

  return 0;
}
