#ifndef MOTION_SENSOR_H
#define MOTION_SENSOR_H

/* Event callback types */
typedef void (*motion_sensor_on_trigger_cb_t)(int pin, int on);

/* Event handlers */
void motion_sensor_set_on_trigger(motion_sensor_on_trigger_cb_t cb);

int motion_sensor_initialize(int pin);

#endif

