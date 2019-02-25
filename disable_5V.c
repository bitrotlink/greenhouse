#include <wiringPi.h>

int main (void)
{
  wiringPiSetupGpio();
  pinMode (27, OUTPUT);
  digitalWrite (27,  LOW);
  return 0;
}
