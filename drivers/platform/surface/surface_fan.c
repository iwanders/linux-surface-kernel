// SPDX-License-Identifier: GPL-2.0+
/*
 * Surface Fan driver for Surface System Aggregator Module.
 *
 * Copyright (C) 2023 Ivor Wanders <ivor@iwanders.net>
 */

#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>

#include <linux/thermal.h>
#include <linux/surface_aggregator/device.h>

// This is a non-SSAM client driver as per the documentation.
// It registers itself as an ACPI driver for PNP0C0B, which requires removing
// the default fan module. It then sets up a thermal cooling device.


// https://docs.kernel.org/driver-api/surface_aggregator/client.html
// https://docs.kernel.org/driver-api/thermal/sysfs-api.html


struct fan_data {
	struct device *dev;
	struct acpi_device *acpi_fan;
	struct ssam_controller *ctrl;

	struct thermal_cooling_device *cdev;
	//struct acpi_connection_info info;
};

// SSAM
SSAM_DEFINE_SYNC_REQUEST_W(__ssam_fan_set, __le16, {
        .target_category = SSAM_SSH_TC_FAN,
        .target_id       = SSAM_SSH_TID_SAM,
        .command_id      = 0x0b,
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
	__le16 value = clamp(state, 0lu, (1lu << 16));
	__ssam_fan_set(d->ctrl, &value);
	return 0;
}

static int surface_fan_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	printk(KERN_INFO "surface_fan_get_cur_state.\n");
	*state = 111;
	return 0;
}

static int surface_fan_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	printk(KERN_INFO "surface_fan_get_max_state.\n");
	*state = 13370;
	return 0;
}

static const struct thermal_cooling_device_ops surface_fan_cooling_ops = {
	.get_max_state = surface_fan_get_max_state,
	.get_cur_state = surface_fan_get_cur_state,
	.set_cur_state = surface_fan_set_cur_state,
};


static int surface_fan_probe(struct platform_device *pdev)
{
	struct acpi_device *acpi_fan = ACPI_COMPANION(&pdev->dev);

	struct ssam_controller *ctrl;
	struct fan_data *data;
	struct thermal_cooling_device *cdev;


	printk(KERN_INFO "probing register.\n");
	dev_dbg(&pdev->dev, "probing\n");

	ctrl = ssam_client_bind(&pdev->dev);
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl) == -ENODEV ? -EPROBE_DEFER : PTR_ERR(ctrl);

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;


	cdev = thermal_cooling_device_register("fan",
					data, &surface_fan_cooling_ops);
	if (IS_ERR(cdev))
		return PTR_ERR(cdev) == -ENODEV ? -EPROBE_DEFER : PTR_ERR(cdev);


	data->dev = &pdev->dev;
	data->ctrl = ctrl;
	data->acpi_fan = acpi_fan;
	data->cdev = cdev;

	// We should probably actually probe if we have the fan!

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
