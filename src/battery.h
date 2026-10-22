#ifndef BATTERY_H
#define BATTERY_H

#include "config.h"

int battery_init(void);
uint16_t battery_read_mv(void);
uint8_t battery_get_percentage(void);
uint8_t battery_get_percentage_from_mv(uint16_t mv);

#endif // BATTERY_H
