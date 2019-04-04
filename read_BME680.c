#include <string.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <bme680.h>
#include "gh_ctrl.h"

#define DEBUG_PRINT false
#define DEBUG_PRINT_RAW false

#define BME680_I2C_ADDRESS 0x77
#define BUF_SIZE 256
#define READ_PERIOD 5 // seconds. Just to avoid tying up the I2C bus too much.
#define WARM_UP_CYCLES 100

char zSql_log[] = "insert into BME680_logs values (?,?,?,?,?,?)";
const char* sensor_type = "BME680";
int fd;

bool temp_prev_inc_p, pres_prev_inc_p, hum_prev_inc_p, gas_prev_inc_p;
int temp_mem, pres_mem, hum_mem, gas_mem;

struct bme680_dev sensor;

void check_error(uint8_t rslt) {
  if(rslt!=BME680_OK) {
    fprintf(stderr, "BME680: OOPS: code %d\n", rslt);
    exit(rslt);
  }
}

// Callbacks
void user_delay_ms(uint32_t period) {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = period*1000000;
  nanosleep(&ts, NULL);
}

int8_t user_i2c_read(uint8_t dev_id, uint8_t reg_addr, uint8_t *reg_data, uint16_t len) {
  struct i2c_msg m_read_set[] = {
    {
      .addr = BME680_I2C_ADDRESS,
      .buf = (void*)(&reg_addr),
      .len = 1,
    }
  };
   struct i2c_msg m_read_get[] = {
    {
      .addr = BME680_I2C_ADDRESS,
      .flags = I2C_M_RD,
      .buf = (void*)reg_data,
      .len = len,
    }
  };
  struct i2c_rdwr_ioctl_data d_read_set = {
    .msgs = m_read_set,
    .nmsgs = 1,
  };
  struct i2c_rdwr_ioctl_data d_read_get = {
    .msgs = m_read_get,
    .nmsgs = 1,
  };
  int8_t ret = ioctl(fd, I2C_RDWR, &d_read_set);
  if(ret<0) return ret;
  ret = ioctl(fd, I2C_RDWR, &d_read_get);
  return(ret<0?ret:0);
}

int8_t user_i2c_write(uint8_t dev_id, uint8_t reg_addr, uint8_t *reg_data, uint16_t len) {
  uint8_t buf[BUF_SIZE];
  if(len+1>BUF_SIZE) {
    fprintf(stderr, "Tried to write %d bytes using a %d-byte buffer\n", len, BUF_SIZE);
    exit(-1);
  }
  buf[0] = reg_addr;
  for(int i=0; i<len; i++) buf[i+1]=reg_data[i]; // XXX Grotesquely retarded
  struct i2c_msg m_write[] = {
    {
      .addr = BME680_I2C_ADDRESS,
      .buf = (void*)buf,
      .len = len+1,
    },
  };
  struct i2c_rdwr_ioctl_data d_write = {
    .msgs = m_write,
    .nmsgs = 1,
  };
  int8_t ret = ioctl(fd, I2C_RDWR, &d_write);
  return(ret<0?ret:0);
}

// Adapted from https://github.com/BoschSensortec/BME680_driver
int8_t sensor_init_config() {
  sensor.dev_id = BME680_I2C_ADDR_SECONDARY;
  sensor.intf = BME680_I2C_INTF;
  sensor.read = user_i2c_read;
  sensor.write = user_i2c_write;
  sensor.delay_ms = user_delay_ms;
  /* amb_temp can be set to 25 prior to configuring the gas sensor
   * or by performing a few temperature readings without operating the gas sensor. */
  sensor.amb_temp = 25;
  int8_t rslt = bme680_init(&sensor);
  check_error(rslt);

  uint8_t set_required_settings;

  /* Set the temperature, pressure and humidity settings */
  sensor.tph_sett.os_hum = BME680_OS_2X;
  sensor.tph_sett.os_pres = BME680_OS_4X;
  sensor.tph_sett.os_temp = BME680_OS_8X;
  sensor.tph_sett.filter = BME680_FILTER_SIZE_3;

  /* Set the remaining gas sensor settings and link the heating profile */
  sensor.gas_sett.run_gas = BME680_ENABLE_GAS_MEAS;
  /* Create a ramp heat waveform in 3 steps */
  sensor.gas_sett.heatr_temp = 320; /* degree Celsius */
  sensor.gas_sett.heatr_dur = 150; /* milliseconds */

  /* Select the power mode */
  /* Must be set before writing the sensor configuration */
  sensor.power_mode = BME680_FORCED_MODE;

  /* Set the required sensor settings needed */
  set_required_settings = BME680_OST_SEL | BME680_OSP_SEL | BME680_OSH_SEL |
    BME680_FILTER_SEL
    | BME680_GAS_SENSOR_SEL;

  /* Set the desired sensor configuration */
  rslt = bme680_set_sensor_settings(set_required_settings,&sensor);
  check_error(rslt);

  /* Set the power mode */
  rslt = bme680_set_sensor_mode(&sensor);
  return rslt;
}


// Adapted from https://github.com/BoschSensortec/BME680_driver
uint8_t read_loop() {
  struct bme680_field_data data;
  int arrData[4];
  int cLoop = 0;
  struct timespec ts;
  ts.tv_sec = READ_PERIOD;
  ts.tv_nsec = 0;
  while(1)
    {
      uint8_t rslt;
      /* Trigger the next measurement if you would like to read data out continuously */
      if (sensor.power_mode == BME680_FORCED_MODE) {
	rslt = bme680_set_sensor_mode(&sensor);
	check_error(rslt);
      }
      /* Get the total measurement duration so as to sleep or wait till the
       * measurement is complete */
      uint16_t meas_period;
      bme680_get_profile_dur(&meas_period, &sensor);
      user_delay_ms(meas_period); /* Delay till the measurement is ready */

      rslt = bme680_get_sensor_data(&data, &sensor);
      check_error(rslt);

      int i_temp = data.temperature * HYST_SCALE / 10;
      int i_pres = data.pressure * HYST_SCALE / 10;
      int i_hum = data.humidity * HYST_SCALE / 100;
      int i_gas = data.gas_resistance * HYST_SCALE / 100000;
      bool temp_changed = reading_change(&temp_prev_inc_p, &temp_mem, i_temp);
      bool pres_changed = reading_change(&pres_prev_inc_p, &pres_mem, i_pres);
      bool hum_changed = reading_change(&hum_prev_inc_p, &hum_mem, i_hum);
      bool gas_changed = reading_change(&gas_prev_inc_p, &gas_mem, i_gas);
      if(DEBUG_PRINT) {
	if(DEBUG_PRINT_RAW) {
	  printf("T: %.2f degC, P: %.2f hPa, H %.2f %%rH ", data.temperature / 100.0f,
		 data.pressure / 100.0f, data.humidity / 1000.0f );
	  /* Avoid using measurements from an unstable heating setup */
	  if(data.status & BME680_GASM_VALID_MSK)
	    printf(", G: %d ohms", data.gas_resistance);
	  printf(" %d\n", cLoop);
	} else {
	  if((cLoop >= WARM_UP_CYCLES) && (temp_changed || pres_changed || hum_changed || gas_changed))
	    printf("T: %d cC, P: %d daPa, H: %d permil, G: %d kohms, %d\n",
		   temp_mem*10/HYST_SCALE,
		   pres_mem/HYST_SCALE,
		   hum_mem/HYST_SCALE,
		   gas_mem*100/HYST_SCALE,
		   cLoop);
	}
      }
      if((cLoop >= WARM_UP_CYCLES) && (temp_changed || pres_changed || hum_changed || gas_changed)) {
	arrData[0] = temp_mem*10/HYST_SCALE;
	arrData[1] = pres_mem/HYST_SCALE;
	arrData[2] = hum_mem/HYST_SCALE;
	arrData[3] = gas_mem*100/HYST_SCALE;
	insert_record(pStmt_log, arrData, 4);
      } else update_idle_heartbeat();
      if(cLoop >= WARM_UP_CYCLES) nanosleep(&ts, NULL);
      cLoop++;
    }
}

void setup() {
  fd = open("/dev/i2c-1", O_RDWR /* | O_NONBLOCK */);
  if(fd==-1) exit(errno);
  if(sensor_init_config()!=BME680_OK) exit(-1);
}

int main(int argc, char** argv) {
  int rc;
  setup();
  daemon_init(argc, argv);
  read_loop();
  close(fd);
  return 0;
}
