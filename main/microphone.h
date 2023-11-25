#ifndef MICROPHONE_H
#define MICROPHONE_H

#include <stdint.h>

void microphone_start(void);
void microphone_stop(void);
int microphone_initialize(int clk, int din, uint32_t sample_rate);

#endif
