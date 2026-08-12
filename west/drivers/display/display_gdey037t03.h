#ifndef DRIVERS_DISPLAY_GDEY037T03_H_
#define DRIVERS_DISPLAY_GDEY037T03_H_

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Custom extension API to request full OTP blanking refresh
 * 
 * @param dev Pointer to device structure for the driver instance
 */
void gdey037t03_request_full_refresh(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_DISPLAY_GDEY037T03_H_ */
