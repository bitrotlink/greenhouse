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
#include "gh_ctrl.h"

#define DEBUG_PRINT false
#define DEBUG_PRINT_RAW false

#define SHT31_I2C_ADDRESS 0x44
#define CONV_COMMAND 0x0024 // Command is 0x2400, but the chip reads big endian
#define INTEGRATION_TIME 15000000 // ns (i.e. 15ms)
#define LOGGING_PERIOD 5 // seconds

const char zSql_log[] = "insert into SHT31_logs values (?,?,?,?)";
const char* sensor_type = "SHT31";
int fd;
bool temp_prev_inc_p, hum_prev_inc_p;
int temp_mem, hum_mem;

void setup() {
  fd = open("/dev/i2c-1", O_RDWR /* | O_NONBLOCK */);
  if(fd==-1) exit(errno);
}

int crc(uint8_t* m) {
  uint8_t rem = 0xff;
  uint8_t bit, byte;
  for(byte=0; byte<2; byte++) {
    rem ^= (m[byte]);
    for(bit=8; bit>0; bit--) {
      if(rem & 0x80) rem = (rem << 1) ^ 0x31;
      else rem = (rem << 1);
    }
  }
  return rem;
}

void read_all() {
  uint16_t cmd = CONV_COMMAND;
  struct i2c_msg m_write[] = {
    {
      .addr = SHT31_I2C_ADDRESS,
      .buf = (void*)(&cmd),
      .len = sizeof(cmd),
    },
  };
    struct i2c_rdwr_ioctl_data d_write = {
    .msgs = m_write,
    .nmsgs = 1,
    };
    int ret = ioctl(fd, I2C_RDWR, &d_write);
    if(ret < 0) exit(errno);
  uint8_t buf[6];
  struct i2c_msg m_read[] = {
    {
      .addr = SHT31_I2C_ADDRESS,
      .flags = I2C_M_RD,
      .buf = (void*)buf,
      .len = sizeof(buf),
    },
  };
  struct i2c_rdwr_ioctl_data d_read = {
    .msgs = m_read,
    .nmsgs = 1,
  };
  struct timespec ts;
  ts.tv_sec = LOGGING_PERIOD;
  // ts.tv_nsec = max(LOGGING_PERIOD, INTEGRATION_TIME); // When INTEGRATION_TIME is ns instead of seconds
  ts.tv_nsec = 0;
  nanosleep(&ts, NULL); // Allow time for conversion to complete
  ret = ioctl(fd, I2C_RDWR, &d_read);
  if(ret != 1)
    fprintf(stderr, "SHT31 read failed with error code %d\n", ret);
  else { // Read succeeded
    uint16_t temp = buf[1] + (buf[0]<<8);
    uint16_t hum = buf[4] + (buf[3]<<8);
    uint8_t temp_crc = crc(buf);
    uint8_t hum_crc = crc(buf+3);
    if((temp_crc!=*(buf+2)) || (hum_crc!=*(buf+5)))
      fprintf(stderr, "SHT31 CRC error\n");
    else { // Readings are good
      int i_temp = (-45 * HYST_SCALE * 100)
	+ (int)temp * 175 * HYST_SCALE * 100 / 65535;
      int i_hum = (int)hum * HYST_SCALE * 1000 / 65535;
      bool temp_changed = reading_change(&temp_prev_inc_p, &temp_mem, i_temp);
      bool hum_changed = reading_change(&hum_prev_inc_p, &hum_mem, i_hum);
      int arrData[2];
      arrData[0] = temp_mem/HYST_SCALE;
      arrData[1] = hum_mem/HYST_SCALE;
      if(DEBUG_PRINT) {
	if(DEBUG_PRINT_RAW) {
	  double temp_f = -45 + 175*((double)temp/65535);
	  double hum_f = ((double)hum)/65535;
	  printf("temp: %.3f, hum: %.3f\n", temp_f, hum_f);
	} else {
	  printf("temp: %d, hum: %d %d %d\n", arrData[0], arrData[1], temp_changed, hum_changed);
	}
      }
      if(temp_changed || hum_changed) insert_record(pStmt_log, arrData, 2);
      else update_idle_heartbeat();
    }
  }
}

int main(int argc, char** argv) {
  setup();
  daemon_init(argc, argv);
  while(1) {
    read_all();
  }
  close(fd);
  return 0;
}
