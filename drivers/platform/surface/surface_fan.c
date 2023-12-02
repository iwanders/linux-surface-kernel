// SPDX-License-Identifier: GPL-2.0+
/*
 * Surface Fan driver for Surface System Aggregator Module.
 *
 * Copyright (C) 2023 Ivor Wanders <ivor@iwanders.net>
 */

#include <linux/acpi.h>

#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/surface_aggregator/device.h>
#include <linux/thermal.h>
#include <linux/types.h>

// This is a non-SSAM client driver as per the surface documentation.
// It registers itself as an ACPI driver for PNP0C0B, which requires removing
// the default fan module.
// It both sets up as a cooling device through the thermal cooling device as
// well as a hwmon fan for monitoring.

// Min fan speed is 2000, max is roughly 7140.

// https://docs.kernel.org/driver-api/surface_aggregator/client.html
// https://docs.kernel.org/driver-api/thermal/sysfs-api.html
// https://docs.kernel.org/hwmon/sysfs-interface.html


struct fan_data {
	struct device *dev;
	struct ssam_controller *ctrl;
	struct acpi_device *acpi_fan;

	struct thermal_cooling_device *cdev;
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

// ACPI
static const struct acpi_device_id surface_fan_match[] = {
	{ "PNP0C0B" },
	{},
};
MODULE_DEVICE_TABLE(acpi, surface_fan_match);



// Thermal cooling device
static int surface_fan_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct fan_data *d = cdev->devdata;
	__le16 value = cpu_to_le16(clamp(state, 0lu, (1lu << 16)));
	return __ssam_fan_set(d->ctrl, &value);
}

static int surface_fan_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	int res;
	printk(KERN_INFO "surface_fan_get_cur_state.\n");
	struct fan_data *d = cdev->devdata;
	__le16 value = 0;
	res = __ssam_fan_get(d->ctrl, &value);
	*state = le16_to_cpu(value);
	return res;
}

static int surface_fan_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	printk(KERN_INFO "surface_fan_get_max_state.\n");
	*state = 7200; // My fan tops out at like 7140.
	return 0;
}

static const struct thermal_cooling_device_ops surface_fan_cooling_ops = {
	.get_max_state = surface_fan_get_max_state,
	.get_cur_state = surface_fan_get_cur_state,
	.set_cur_state = surface_fan_set_cur_state,
};


// hwmon
umode_t surface_fan_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type,
	u32 attr, int channel) {
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
		case hwmon_fan_min:
		case hwmon_fan_max:
			return 0444;
		default:
			break;
		}
	default:
		break;
	}
	return 0;
}

static int surface_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			 long *val)
{
	struct fan_data *data = dev_get_drvdata(dev);
	__le16 value;
	int res;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			res = __ssam_fan_get(data->ctrl, &value);
			if (res != 0) {
				return -1;
			} 
			*val = le16_to_cpu(value);
			return 0;
		case hwmon_fan_min:
			*val = 2000;
			return 0;
		case hwmon_fan_max:
			*val = 7200;
			return 0;
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
	struct fan_data *data = dev_get_drvdata(dev);
	return 0;
}

static const struct hwmon_channel_info * const surface_fan_info[] = {
	HWMON_CHANNEL_INFO(fan,
				HWMON_F_INPUT |
				HWMON_F_MAX |
				HWMON_F_MIN
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

static int surface_fan_probe(struct platform_device *pdev)
{
	struct acpi_device *acpi_fan = ACPI_COMPANION(&pdev->dev);

	struct ssam_controller *ctrl;
	struct fan_data *data;
	struct thermal_cooling_device *cdev;
	struct device *hdev;
	__le16 value;
	int status;


	printk(KERN_INFO "probing register.\n");
	dev_dbg(&pdev->dev, "probing\n");

	ctrl = ssam_client_bind(&pdev->dev);
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl) == -ENODEV ? -EPROBE_DEFER : PTR_ERR(ctrl);

	// Probe the fan to confirm we actually have it by retrieving the 
	// speed.
	status = __ssam_fan_get(ctrl, &value);
	if (status) {
		dev_err(&pdev->dev, "Failed to probe fan speed: %d\n", status);
		return -ENODEV;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	cdev = thermal_cooling_device_register("Fan",
					data, &surface_fan_cooling_ops);
	if (IS_ERR(cdev))
		return PTR_ERR(cdev) == -ENODEV ? -EPROBE_DEFER : PTR_ERR(cdev);


	hdev = devm_hwmon_device_register_with_info(&pdev->dev, "fan", data,
							&surface_fan_chip_info,
							NULL);
	if (IS_ERR(hdev)) {
		printk(KERN_INFO "hdev dregistration fail register.\n");
		return PTR_ERR(hdev) == -ENODEV ? -EPROBE_DEFER : PTR_ERR(hdev);
	}

	data->dev = &pdev->dev;
	data->ctrl = ctrl;
	data->acpi_fan = acpi_fan;

	data->cdev = cdev;
	data->hdev = hdev;


	platform_set_drvdata(pdev, data);

	printk(KERN_INFO "Yay.\n");
	return 0;
}

static int surface_fan_remove(struct platform_device *pdev)
{
	struct fan_data *d = platform_get_drvdata(pdev);
	thermal_cooling_device_unregister(d->cdev);
	dev_dbg(&pdev->dev, "remove\n");
	return 0;
}


static struct platform_driver surface_fan = {
	.probe = surface_fan_probe,
	.remove = surface_fan_remove,
	.driver = {
		.name = "surface_fan",
		.acpi_match_table = surface_fan_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static int __init surface_fan_init(void)
{
	int ret;
	printk(KERN_INFO "Trying register.\n");
	ret = platform_driver_register(&surface_fan);
	printk(KERN_INFO "Ret: %d.\n", ret);
	return ret;
}
module_init(surface_fan_init);

static void __exit surface_fan_exit(void)
{
	printk(KERN_INFO "Bye: .\n");
	platform_driver_unregister(&surface_fan);
}
module_exit(surface_fan_exit);

MODULE_AUTHOR("Ivor Wanders <ivor@iwanders.net>");
MODULE_DESCRIPTION("Fan Driver for Surface System Aggregator Module");

MODULE_LICENSE("GPL");
