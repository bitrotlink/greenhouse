#include <wiringPiI2C.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "gh_ctrl.h"

#define PRINT_DEBUG false
#define PRINT_DEBUG_RAW false

#define VEML6075_I2C_ADDRESS 0x10
#define UV_CONF_REGISTER 0
#define UVA_REGISTER 7
#define UVB_REGISTER 9
#define INTEGRATION_TIME 50000000 // ns (i.e. 50ms)
#define LOGGING_PERIOD 500000000 // ns (i.e. 500ms)

// Sensitivity coefficients, in (μW/cm²)/count
#define UVA_COEF ((double)1/0.93)
#define UVB_COEF ((double)1/2.1)

char zSql_log[] = "insert into VEML6075_logs values (?,?,?,?)";
const char* sensor_type = "VEML6075";
int fd_veml6075;

bool uva_prev_inc_p, uvb_prev_inc_p;
int uva_mem, uvb_mem;

void setup() {
  fd_veml6075 = wiringPiI2CSetup(VEML6075_I2C_ADDRESS);
  if(fd_veml6075==-1) exit(errno);
  wiringPiI2CWriteReg16(fd_veml6075, UV_CONF_REGISTER, 0);
}

void read_all() {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = max(LOGGING_PERIOD, INTEGRATION_TIME * 2); // *2 in case the datasheet lies about the actual conversion time, as is the case for the TSL2591
  nanosleep(&ts, NULL); // Allow time for conversion to complete
  uint uva = wiringPiI2CReadReg16(fd_veml6075, UVA_REGISTER);
  uint uvb = wiringPiI2CReadReg16(fd_veml6075, UVB_REGISTER);
  double uva_power = uva * UVA_COEF/100; // /100 to scale from μW/cm² to W/m²
  double uvb_power = uvb * UVB_COEF/100;
  int i_uva = uva_power * HYST_SCALE *1000;
  int i_uvb = uvb_power * HYST_SCALE *1000;
  bool uva_changed = reading_change(&uva_prev_inc_p, &uva_mem, i_uva);
  bool uvb_changed = reading_change(&uvb_prev_inc_p, &uvb_mem, i_uvb);
  int arrData[2];
  arrData[0] = uva_mem/HYST_SCALE;
  arrData[1] = uvb_mem/HYST_SCALE;
  if(PRINT_DEBUG) {
    if(PRINT_DEBUG_RAW) printf("UVA 0x%x %.3f  UVB 0x%x %.3f\n", uva, uva_power, uvb, uvb_power);
    else printf("%d uva %d uvb %d %d\n", arrData[0], arrData[1], uva_changed, uvb_changed);
  }
  if(uva_changed || uvb_changed) insert_record(pStmt_log, arrData, 2);
  else update_idle_heartbeat();
}

int main(int argc, char** argv) {
  int rc;
  setup();
  daemon_init(argc, argv);
  while(1) {
    read_all();
  }
  return 0;
}
