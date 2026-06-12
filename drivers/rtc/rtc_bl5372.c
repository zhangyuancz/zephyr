/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT belling_bl5372

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "rtc_utils.h"

LOG_MODULE_REGISTER(bl5372, CONFIG_RTC_LOG_LEVEL);

#define BL5372_REG_SECONDS 0x0U
#define BL5372_REG_CONTROL2 0xFU

#define BL5372_SECONDS_MASK GENMASK(6, 0)
#define BL5372_MINUTES_MASK GENMASK(6, 0)
#define BL5372_HOURS_MASK GENMASK(5, 0)
#define BL5372_WEEKDAY_MASK GENMASK(2, 0)
#define BL5372_MONTHDAY_MASK GENMASK(5, 0)
#define BL5372_MONTH_MASK GENMASK(4, 0)

#define BL5372_CONTROL2_24H BIT(5)
#define BL5372_CONTROL2_XSTP BIT(4)
#define BL5372_CONTROL2_CLEN BIT(3)

#define BL5372_TRANSFER_WRITE 0x0U

#define BL5372_TIME_MASK                                                                          \
	(RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE | RTC_ALARM_TIME_MASK_HOUR |      \
	 RTC_ALARM_TIME_MASK_MONTHDAY | RTC_ALARM_TIME_MASK_MONTH | RTC_ALARM_TIME_MASK_YEAR |     \
	 RTC_ALARM_TIME_MASK_WEEKDAY)

#define BL5372_TM_YEAR_2000 100
#define BL5372_TM_YEAR_2099 199

struct bl5372_config {
	struct i2c_dt_spec i2c;
};

struct bl5372_data {
	struct k_mutex lock;
};

static uint8_t bl5372_command(uint8_t reg)
{
	return (reg << 4) | BL5372_TRANSFER_WRITE;
}

static int bl5372_read(const struct bl5372_config *config, uint8_t reg, uint8_t *data,
		       size_t length)
{
	uint8_t command = bl5372_command(reg);

	return i2c_write_read_dt(&config->i2c, &command, sizeof(command), data, length);
}

static int bl5372_write(const struct bl5372_config *config, uint8_t reg, const uint8_t *data,
			size_t length)
{
	uint8_t buffer[8];

	if (length > sizeof(buffer) - 1U) {
		return -EINVAL;
	}

	buffer[0] = bl5372_command(reg);
	memcpy(&buffer[1], data, length);

	return i2c_write_dt(&config->i2c, buffer, length + 1U);
}

static int bl5372_set_time(const struct device *dev, const struct rtc_time *timeptr)
{
	const struct bl5372_config *config = dev->config;
	struct bl5372_data *data = dev->data;
	uint8_t control2 = BL5372_CONTROL2_24H | BL5372_CONTROL2_CLEN;
	uint8_t regs[7];
	int ret;

	if (timeptr == NULL || !rtc_utils_validate_rtc_time(timeptr, BL5372_TIME_MASK) ||
	    timeptr->tm_year < BL5372_TM_YEAR_2000 ||
	    timeptr->tm_year > BL5372_TM_YEAR_2099) {
		return -EINVAL;
	}

	regs[0] = bin2bcd(timeptr->tm_sec) & BL5372_SECONDS_MASK;
	regs[1] = bin2bcd(timeptr->tm_min) & BL5372_MINUTES_MASK;
	regs[2] = bin2bcd(timeptr->tm_hour) & BL5372_HOURS_MASK;
	regs[3] = bin2bcd(timeptr->tm_wday) & BL5372_WEEKDAY_MASK;
	regs[4] = bin2bcd(timeptr->tm_mday) & BL5372_MONTHDAY_MASK;
	regs[5] = bin2bcd(timeptr->tm_mon + 1) & BL5372_MONTH_MASK;
	regs[6] = bin2bcd(timeptr->tm_year - BL5372_TM_YEAR_2000);

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Select 24-hour mode and clear XSTP before writing clock data. */
	ret = bl5372_write(config, BL5372_REG_CONTROL2, &control2, sizeof(control2));
	if (ret == 0) {
		ret = bl5372_write(config, BL5372_REG_SECONDS, regs, sizeof(regs));
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static int bl5372_read_time_regs(const struct bl5372_config *config, uint8_t *regs)
{
	uint8_t previous[7];
	int ret;

	ret = bl5372_read(config, BL5372_REG_SECONDS, previous, sizeof(previous));
	if (ret < 0) {
		return ret;
	}

	for (int attempt = 0; attempt < 2; ++attempt) {
		ret = bl5372_read(config, BL5372_REG_SECONDS, regs, sizeof(previous));
		if (ret < 0) {
			return ret;
		}
		if (memcmp(previous, regs, sizeof(previous)) == 0) {
			return 0;
		}
		memcpy(previous, regs, sizeof(previous));
	}

	return -EAGAIN;
}

static int bl5372_get_time(const struct device *dev, struct rtc_time *timeptr)
{
	const struct bl5372_config *config = dev->config;
	struct bl5372_data *data = dev->data;
	uint8_t control2;
	uint8_t regs[7];
	int ret;

	if (timeptr == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = bl5372_read(config, BL5372_REG_CONTROL2, &control2, sizeof(control2));
	if (ret < 0) {
		goto out;
	}
	if ((control2 & BL5372_CONTROL2_XSTP) != 0U) {
		ret = -ENODATA;
		goto out;
	}
	if ((control2 & BL5372_CONTROL2_24H) == 0U) {
		ret = -ENOTSUP;
		goto out;
	}

	ret = bl5372_read_time_regs(config, regs);
	if (ret < 0) {
		goto out;
	}

	timeptr->tm_sec = bcd2bin(regs[0] & BL5372_SECONDS_MASK);
	timeptr->tm_min = bcd2bin(regs[1] & BL5372_MINUTES_MASK);
	timeptr->tm_hour = bcd2bin(regs[2] & BL5372_HOURS_MASK);
	timeptr->tm_wday = bcd2bin(regs[3] & BL5372_WEEKDAY_MASK);
	timeptr->tm_mday = bcd2bin(regs[4] & BL5372_MONTHDAY_MASK);
	timeptr->tm_mon = bcd2bin(regs[5] & BL5372_MONTH_MASK) - 1;
	timeptr->tm_year = bcd2bin(regs[6]) + BL5372_TM_YEAR_2000;
	timeptr->tm_yday = -1;
	timeptr->tm_isdst = -1;
	timeptr->tm_nsec = 0;

	if (!rtc_utils_validate_rtc_time(timeptr, BL5372_TIME_MASK)) {
		ret = -ENODATA;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static DEVICE_API(rtc, bl5372_api) = {
	.set_time = bl5372_set_time,
	.get_time = bl5372_get_time,
};

static int bl5372_init(const struct device *dev)
{
	const struct bl5372_config *config = dev->config;
	struct bl5372_data *data = dev->data;

	if (!i2c_is_ready_dt(&config->i2c)) {
		return -ENODEV;
	}

	k_mutex_init(&data->lock);
	return 0;
}

#define BL5372_DEFINE(inst)                                                                        \
	static struct bl5372_data bl5372_data_##inst;                                               \
	static const struct bl5372_config bl5372_config_##inst = {                                  \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                   \
	};                                                                                           \
	DEVICE_DT_INST_DEFINE(inst, bl5372_init, NULL, &bl5372_data_##inst,                          \
			      &bl5372_config_##inst, POST_KERNEL, CONFIG_RTC_INIT_PRIORITY,          \
			      &bl5372_api);

DT_INST_FOREACH_STATUS_OKAY(BL5372_DEFINE)
