#include <wiringPi.h>

int main (void)
{
  wiringPiSetupGpio();
  pinMode (17, OUTPUT);
  pinMode (27, OUTPUT);
  digitalWrite (17, HIGH);
  digitalWrite (27,  HIGH);
  return 0;
}
