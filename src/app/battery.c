#include "battery.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/pm/pm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

// ADC Channel Config
// Input logic: Measure VDD directly if possible via Internal Input.
// Gain 1/6, Ref Internal (0.6V), 10 bit.
// Max Input: 0.6V * 6 = 3.6V.
// Range: 0..1023 maps to 0..3.6V

static struct adc_channel_cfg channel_cfg = {
    .gain = ADC_GAIN_1_6,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .input_positive = SAADC_CH_PSELP_PSELP_VDD // Measure VDD
};

static int16_t sample_buffer[1];
static struct adc_sequence sequence = {
    .channels = BIT(0),
    .buffer = sample_buffer,
    .buffer_size = sizeof(sample_buffer),
    .resolution = 10,
};

int battery_init(void) {
    if (!device_is_ready(adc_dev)) {
        LOG_ERR("ADC device not ready");
        return -ENODEV;
    }
    
    int err = adc_channel_setup(adc_dev, &channel_cfg);
    if (err) {
        LOG_ERR("ADC channel setup failed: %d", err);
        return err;
    }

    return 0;
}

int battery_read_mv(void) {
    if (!adc_dev) return 0;
    
    int err = adc_read(adc_dev, &sequence);
    if (err) {
        LOG_ERR("ADC read failed: %d", err);
        return 0;
    }

    // 10 bit resolution, Ref 0.6V, Gain 1/6.
    // Val = (Input / 3.6) * 1023
    // Input = Val * 3.6 / 1023
    // mV = Val * 3600 / 1023
    int32_t val = sample_buffer[0];
    if (val < 0) val = 0;
    
    return (val * 3600) / 1023;
}
