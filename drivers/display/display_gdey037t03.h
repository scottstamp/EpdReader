#ifndef DRIVERS_DISPLAY_GDEY037T03_H_
#define DRIVERS_DISPLAY_GDEY037T03_H_

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

#ifdef __cplusplus
extern "C" {
#endif

// Partial refresh waveforms (register LUT). Duration scales with interval x pulses; the
// frame rate is the controller default except in CUSTOM. Full refreshes always use OTP.
enum gdey037t03_refresh_profile {
    GDEY037T03_PROFILE_NORMAL = 0, // interval 2, 3 pulses
    GDEY037T03_PROFILE_FAST   = 1, // interval 2, 2 pulses
    GDEY037T03_PROFILE_TURBO  = 2, // interval 1, 1 pulse
    GDEY037T03_PROFILE_CUSTOM = 3, // user pulse interval, pulse count and frame rate
};

struct gdey037t03_refresh_config {
    enum gdey037t03_refresh_profile profile;
    uint8_t pulse_interval; // 1 - 15 (frames per pulse phase)
    uint8_t pulse_count;    // 1 - 5 (number of pulse iterations / repeat count)
    uint8_t frame_rate;     // CUSTOM only: Hz written to PLL (0x30), 5 - 200; 0 = controller default
};

/**
 * @brief Custom extension API to request full OTP blanking refresh
 * 
 * @param dev Pointer to device structure for the driver instance
 */
void gdey037t03_request_full_refresh(const struct device *dev);

/**
 * @brief Configure waveform refresh timing profile, pulse interval, and pulse count
 */
int gdey037t03_set_refresh_config(const struct device *dev, const struct gdey037t03_refresh_config *cfg);

/**
 * @brief Query current waveform refresh timing configuration
 */
int gdey037t03_get_refresh_config(const struct device *dev, struct gdey037t03_refresh_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_DISPLAY_GDEY037T03_H_ */
