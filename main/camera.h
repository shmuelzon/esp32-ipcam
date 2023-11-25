#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

void camera_start(void);
void camera_stop(void);

int camera_initialize(int pwdn, int reset, int xclk, int siod, int sioc, int d7,
    int d6, int d5, int d4, int d3, int d2, int d1, int d0, int vsync, int href,
    int pclk, const char *resolution, int fps, uint8_t vflip, uint8_t hmirror,
    int quality);

#endif
