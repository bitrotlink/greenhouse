#include <wiringPi.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

//RPi BCM pins
#define SCLK 23
#define DOUT 24

// MAX11201B updates at 13.75 sps, but polling slower than that introduces occasional glitched data due to the chip unilaterally pulsing DOUT high to indicate a new conversion has been performed, which introduces a race condition between verifying DOUT is low and initiating a read, and the glitch can't be definitively filtered out since there's no CRC. And interrupts are impractical because wiringPi is crap.
// The solution is to poll faster than the update period, so the chip never gets a chance to send its unilateral pulse.
#define POLL_PERIOD (40*1000*1000) // ns
#define UPDATE_PERIOD ((double)1/13.75) // sec

#define ONE_SECOND (1000*1000*1000) // ns
#define RECALIBRATION_PERIOD 10 // Unit is calls to ADC_read
#define EMA_ALPHA 0.02 // For exponential moving average
#define SPIN_UP_DELAY ((double)5/EMA_ALPHA) // Unit is calls to ADC_read
#define SPIN_UP_DELAY_SEC (SPIN_UP_DELAY*POLL_PERIOD/ONE_SECOND)
#define SPIN_UP_DELAY_SEC_LIMITED (SPIN_UP_DELAY*UPDATE_PERIOD) // Approximation, since polling will delay for another full POLL_PERIOD each time ADC isn't ready yet

#define SCLK_PERIOD 10000 // ns. 100kHz is adequate

#define REFP 3.3 // ADC positive reference voltage
#define FRAC_SCALE 8388608 // 2^23; ADC is 24-bit two's complement
#define MICROV_SCALE ((double)1000000*REFP/FRAC_SCALE)

double ema_accum;
bool reading;

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
  int val_accum=0;
  half_tick.tv_sec = 0;
  half_tick.tv_nsec = SCLK_PERIOD/2;
  for(int i=0; i<24; i++) {
    int x;
    digitalWrite(SCLK, 1);
    nanosleep(&half_tick, NULL);
    digitalWrite(SCLK, 0);
    x=digitalRead(DOUT);
    if(x && (i==0)) val_accum=-1; //First bit is sign bit
    else {
      val_accum<<=1;
      val_accum|=x;
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
  ema_accum=(EMA_ALPHA*(double)val_accum)+(1.0 - EMA_ALPHA)*ema_accum;
  return val_accum;
  // And there's no CRC. Great job, Maxim... :-(
}

void output_val(int val) {
  double f_val = (double)val;
  double frac=(f_val)/FRAC_SCALE; //Range -1 to 1
  printf("0x%8X  %12.6f/1  %12.6fμV  %12.6fμV EMA\n", val, frac, f_val*MICROV_SCALE, ema_accum*MICROV_SCALE);
}

void ready_interrupt() {
  int val;
  if(reading) return; //Ready line is same as data line (RDY/DOUT), so ignore interrupt while reading
  val=digitalRead(DOUT);
  if(val) return; //XXX: Spurious interrupt. Happens after every read; seems the wiringPi library delays the ISR call for a new interrupt until the previous call returns. Which also means the reading mutex is unnecessary.
  reading=true;
  val=ADC_read();
  output_val(val);
  reading=false;
}

//Using interrupts produces 100% CPU usage, even in test case with do-nothing ISR and only 14 interrupts per second. Crapware kernel or libraries.
//And at least wiringPi is crapware, apparently providing no way to unregister the ISR to avoid triggering when the MAX11201B RDY/DOUT pin is being used as DOUT (triggering is only needed when the pin is used as RDY).
void use_interrupts() {
  reading=false;
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
  fprintf(stderr, "Spin-up delay approx %ds...\n", (int)SPIN_UP_DELAY_SEC_LIMITED);
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

int main() {
  wiringPiSetupGpio();
  pinMode(SCLK, OUTPUT);
  pullUpDnControl(DOUT, PUD_UP);
  ADC_calibrate();
  ema_accum=0;
  //use_interrupts();
  use_polling();
  return 0;
}
