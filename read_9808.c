#include <wiringPiI2C.h>
#include <errno.h>
#include <stdio.h>

int main (void)
{
  int fd9808 = wiringPiI2CSetup (0x18) ;
  if(fd9808==-1) return errno;
  int tmpr = wiringPiI2CReadReg16(fd9808, 5);
  int upper = (tmpr&0x000f)<<8;
  int sign = upper & 0x1000;
  if(sign) upper|=(-1>>12)<<12;
  int lower = (tmpr&0xff00)>>8;
  printf("raw: %x\nlower:%x\nupper:%x\n%f\n", tmpr, lower, upper, (float)(upper+lower)/16);
  return 0 ;
}
