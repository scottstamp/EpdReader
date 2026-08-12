#ifndef BATTERY_H
#define BATTERY_H

#include "config.h"

int battery_init(void);
uint16_t battery_read_mv(void);
uint8_t battery_get_percentage(void);

#endif // BATTERY_H
