// SPDX-License-Identifier: GPL-2.0+
/*
 * Surface Fan driver for Surface System Aggregator Module.
 *
 * Copyright (C) 2023 Ivor Wanders <ivor@iwanders.net>
 */

#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/surface_aggregator/device.h>
#include <linux/thermal.h>
#include <linux/types.h>

// This is a SSAM client driver as per the surface documentation.
// It both sets up as a cooling device through the thermal cooling device as
// well as a hwmon fan for monitoring.

// This driver can change the fan speed, but only while the onboard controller
// is not overriding it. At about 40 degrees celsius that takes over and over
// writes whatever setpoint was given.

#define SURFACE_FAN_MIN_SPEED 2000
#define SURFACE_FAN_MAX_SPEED 8000

struct fan_data {
	struct device *dev;
	struct thermal_cooling_device *cdev;

	struct ssam_device *sdev;

	struct device *hdev;
};

// SSAM
SSAM_DEFINE_SYNC_REQUEST_W(__ssam_fan_set, __le16, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id       = SSAM_SSH_TID_SAM,
	.command_id      = 0x0b,
	.instance_id     = 0x01,
});

SSAM_DEFINE_SYNC_REQUEST_R(__ssam_fan_get, __le16, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id       = SSAM_SSH_TID_SAM,
	.command_id      = 0x01,
	.instance_id     = 0x01,
});


// Thermal cooling device
static int surface_fan_set_cur_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	__le16 value;
	struct fan_data *d = cdev->devdata;
	value = cpu_to_le16(clamp(state, 0lu, (1lu << 16)));
	return __ssam_fan_set(d->sdev->ctrl, &value);
}

static int surface_fan_get_cur_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	int res;
	struct fan_data *d = cdev->devdata;
	__le16 value = 0;
	res = __ssam_fan_get(d->sdev->ctrl, &value);
	*state = le16_to_cpu(value);
	return res;
}

static int surface_fan_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	*state = SURFACE_FAN_MAX_SPEED;
	return 0;
}

static const struct thermal_cooling_device_ops surface_fan_cooling_ops = {
	.get_max_state = surface_fan_get_max_state,
	.get_cur_state = surface_fan_get_cur_state,
	.set_cur_state = surface_fan_set_cur_state,
};


// hwmon
umode_t surface_fan_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type,
					u32 attr, int channel) {
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
		case hwmon_fan_min:
		case hwmon_fan_max:
			return 0444;
		case hwmon_fan_target:
			return 0644;
		default:
			break;
		}
	default:
		break;
	}
	return 0;
}

static int surface_fan_hwmon_read(struct device *dev,
					enum hwmon_sensor_types type,
					u32 attr, int channel, long *val)
{
	struct fan_data *d = dev_get_drvdata(dev);
	__le16 value;
	int res;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			res = __ssam_fan_get(d->sdev->ctrl, &value);
			if (res) {
				return -1;
			} 
			*val = le16_to_cpu(value);
			return 0;
		case hwmon_fan_min:
			*val = SURFACE_FAN_MIN_SPEED;
			return 0;
		case hwmon_fan_max:
			*val = SURFACE_FAN_MAX_SPEED;
			return 0;
		case hwmon_fan_target:
			// No known way to retrieve the current setpoint.
			return -1;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -1;
}


static int surface_fan_hwmon_write(struct device *dev,
					enum hwmon_sensor_types type,
					u32 attr, int channel, long val)
{
	__le16 value;
	struct fan_data *d = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_target:
			value = cpu_to_le16(clamp(val, 0l, (1l << 16)));
			return __ssam_fan_set(d->sdev->ctrl, &value);
		default:
			break;
		}
		break;
	default:
		break;
	}
	return -1;
}

static const struct hwmon_channel_info * const surface_fan_info[] = {
	HWMON_CHANNEL_INFO(fan,
				HWMON_F_INPUT |
				HWMON_F_MAX |
				HWMON_F_MIN |
				HWMON_F_TARGET
				),
	NULL
};

static const struct hwmon_ops surface_fan_hwmon_ops = {
	.is_visible = surface_fan_hwmon_is_visible,
	.read = surface_fan_hwmon_read,
	.write = surface_fan_hwmon_write,
};

static const struct hwmon_chip_info surface_fan_chip_info = {
	.ops = &surface_fan_hwmon_ops,
	.info = surface_fan_info,
};

static int surface_fan_probe(struct ssam_device *sdev)
{
	struct fan_data *data;
	struct thermal_cooling_device *cdev;
	struct device *hdev;
	__le16 value;
	int status;

	// Probe the fan to confirm we actually have it by retrieving the 
	// speed.
	status = __ssam_fan_get(sdev->ctrl, &value);
	if (status) {
		return -ENODEV;
	}

	data = devm_kzalloc(&sdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	cdev = thermal_cooling_device_register("Fan",
					data, &surface_fan_cooling_ops);
	if (IS_ERR(cdev))
		return PTR_ERR(cdev);


	hdev = devm_hwmon_device_register_with_info(&sdev->dev, "fan", data,
							&surface_fan_chip_info,
							NULL);
	if (IS_ERR(hdev)) {
		return PTR_ERR(hdev);
	}

	data->sdev = sdev;
	data->cdev = cdev;
	data->hdev = hdev;

	ssam_device_set_drvdata(sdev, data);

	return 0;
}

static void surface_fan_remove(struct ssam_device *sdev)
{
	struct fan_data *d = ssam_device_get_drvdata(sdev);
	thermal_cooling_device_unregister(d->cdev);
}


static const struct ssam_device_id ssam_fan_match[] = {
	{ SSAM_SDEV(FAN, SAM, SSAM_SSH_IID_ANY, SSAM_SSH_FUN_ANY) },
	{ },
};
MODULE_DEVICE_TABLE(ssam, ssam_fan_match);

static struct ssam_device_driver surface_fan = {
	.probe = surface_fan_probe,
	.remove = surface_fan_remove,
	.match_table = ssam_fan_match,
	.driver = {
		.name = "surface_fan",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_ssam_device_driver(surface_fan);

MODULE_AUTHOR("Ivor Wanders <ivor@iwanders.net>");
MODULE_DESCRIPTION("Fan Driver for Surface System Aggregator Module");
MODULE_LICENSE("GPL");
