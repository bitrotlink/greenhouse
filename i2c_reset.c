#include <wiringPi.h>
int main() {
  wiringPiSetupGpio();

  // Toggle clock
  for (int i=0; i<16; i++) {
    pinMode (3, OUTPUT); digitalWrite(3, LOW); delay(1);
    pinMode (3, INPUT); delay(1); //Don't drive high; bus is open collector
  }

  // Issue STOP condition
  pinMode (3, OUTPUT); digitalWrite(3, LOW); delay(1);
  pinMode (2, OUTPUT); digitalWrite(2, LOW); delay(1);
  pinMode (3, INPUT); delay(1);
  pinMode (2, INPUT);
  return 0;
}
