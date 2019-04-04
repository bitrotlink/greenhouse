#include <wiringPiI2C.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "gh_ctrl.h"

#define PRINT_DEBUG false
#define PRINT_DEBUG_RAW false

#define TSL2591_I2C_ADDRESS 0x29
#define ENABLE_REGISTER 0xa0
#define ENABLE_VALUE 0x13
#define CONFIG_REGISTER 0xa1
#define C0DATAL 0xb4
#define C0DATAH 0xb5
#define C1DATAL 0xb6
#define C1DATAH 0xb7
#define INTEGRATION_TIME 100000000 // ns (i.e. 100ms)
#define ADC_MAX_COUNT 36863 // Per the data sheet, though the chip actually outputs up to 37888 at saturation

// Sensitivity coefficients, in (μW/cm²)/count
#define C0_WHITE_COEF ((double)1/264.1)
#define C0_IRED_COEF ((double)1/257.5)
#define C1_WHITE_COEF ((double)1/34.9)
#define C1_IRED_COEF ((double)1/154.1)

#define LUMENS_PER_WATT 683

#define C1_IRED_WHITE_RATIO ((1/C1_IRED_COEF)/((1/C1_IRED_COEF)+(1/C1_WHITE_COEF)))

double gain_divisor[2][4] = { // First dimension is channel; second is divisor. /400 because specs give sensitivity at AGAIN = High (gain #2)
  {400, (double)400/24.5, 1, (double)400/9200},
  {400, (double)400/24.5, 1, (double)400/9900}};

char zSql_log[] = "insert into TSL2591_logs values (?,?,?,?)";
const char* sensor_type = "TSL2591";
int fd_tsl2591;

bool total_prev_inc_p, ired_prev_inc_p;
int total_mem, ired_mem;

void set_gain(int gain) {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = INTEGRATION_TIME * 2; // *2 since the datasheet lies about the actual time
  wiringPiI2CWriteReg8(fd_tsl2591, CONFIG_REGISTER, gain<<4);
  nanosleep(&ts, NULL); // Allow time for conversion to complete
}

uint get_C(int channel) {
  wiringPiI2CWrite(fd_tsl2591, channel?C1DATAL:C0DATAL);
  uint val_L = wiringPiI2CRead(fd_tsl2591);
  wiringPiI2CWrite(fd_tsl2591, channel?C1DATAH:C0DATAH);
  uint val_H = wiringPiI2CRead(fd_tsl2591);
  return val_L + (val_H << 8);
}

void setup() {
  fd_tsl2591 = wiringPiI2CSetup(TSL2591_I2C_ADDRESS);
  if(fd_tsl2591==-1) exit(errno);
  wiringPiI2CWriteReg8(fd_tsl2591, ENABLE_REGISTER, ENABLE_VALUE);
}

void print_all() {
  for(int g=0; g<4; g++) {
    set_gain(g);
    uint C0 = get_C(0);
    uint C1 = get_C(1);
    printf("Gain %d  C0 0x%x  C1 0x%x\n", g, C0, C1);
  }
}

void read_and_AGC() {
  uint C[2][4] = {{0, 0, 0, 0,}, {0, 0, 0, 0}}; // First dimension is channel; second is gain
  int limit[2] = {0, 0}; // Highest unsaturated gain level for each channel
  for(int g=0; g<4; g++) {
    set_gain(g);
    for(int c=0; c<2; c++) {
      C[c][g] = get_C(c);
      if(C[c][g]<=ADC_MAX_COUNT) limit[c] = g;
    }
  }
  double C0_total = (double)C[0][limit[0]]
    *gain_divisor[0][limit[0]]
    *C0_WHITE_COEF // Total power; C0 response curve covers visible and ired
    /100; // Scale from μW/cm² to W/m²
  double C1_ired = (double)C[1][limit[1]]
    *gain_divisor[1][limit[1]]
    *C1_IRED_COEF*C1_IRED_WHITE_RATIO // Infrared power
    /100;
  double C0_white = C0_total - C1_ired; // C0 curve covers C1 curve
  double lux = C0_white * LUMENS_PER_WATT; // C0_white curve approximates human eye
  int i_total = C0_total * HYST_SCALE *1000000;
  int i_ired = C1_ired * HYST_SCALE *1000000;
  bool total_changed = reading_change(&total_prev_inc_p, &total_mem, i_total);
  bool ired_changed = reading_change(&ired_prev_inc_p, &ired_mem, i_ired);
  int arrData[2];
  arrData[0] = total_mem/HYST_SCALE;
  arrData[1] = ired_mem/HYST_SCALE;
  if(PRINT_DEBUG) {
    if(PRINT_DEBUG_RAW)
      printf("0x%x tot %.6f G %d, 0x%x ired %.6f G %d whit %.6f %.6flx\n",
	     C[0][limit[0]],
	     C0_total,
	     limit[0],
	     C[1][limit[1]],
	     C1_ired,
	     limit[1],
	     C0_white,
	     lux);
    else printf("%d total %d ired %d %d\n", arrData[0], arrData[1], total_changed, ired_changed);
  }
  if(total_changed || ired_changed) insert_record(pStmt_log, arrData, 2);
  else update_idle_heartbeat();
}

int main(int argc, char** argv) {
  setup();
  daemon_init(argc, argv);
  while(1) {
    read_and_AGC();
  }
  return 0;
}
