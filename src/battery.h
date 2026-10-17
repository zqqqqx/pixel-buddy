// Battery: voltage on GPIO1 (ADC1 channel 0, divider), charger status on GPIO41 (same as the stock firmware).
#pragma once

namespace battery {

void init();
void tick();        // every few seconds: one ADC reading, averaged
int level();        // 0..100, -1 = no reading yet / no battery
bool charging();

}  // namespace battery
