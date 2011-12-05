/*
 * Bluetooth Broadcomm  and low power control via GPIO
 *
 *
 * Copyright (C) 2011 Eduardo Jos� Tagle <ejtagle@tutopia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */ 

/* Bluetooth is an HCI UART device attached to UART2, and requires a 32khz blink clock */
 
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <asm/mach-types.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/setup.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/jiffies.h>
#include <linux/rfkill.h>

#include "board-smba1002.h"
#include "gpio-names.h"

#include <mach/hardware.h>
#include <asm/mach-types.h>

struct smba1002_pm_bt_data {
	struct regulator *regulator;
	struct clk *clk;
	struct rfkill *rfkill;
#ifdef CONFIG_PM	
	int pre_resume_state;
	int keep_on_in_suspend;
#endif
	int powered_up;
};

/* Power control */
static void __smba1002_pm_bt_toggle_radio(struct device *dev, unsigned int on)
{
	struct smba1002_pm_bt_data *bt_data = dev_get_drvdata(dev);

	/* Avoid turning it on if already on */
	if (bt_data->powered_up == on)
		return;
	
	if (on) {
		dev_info(dev, "Enabling Bluetooth\n");

		smba1002_wlan_bt_poweron();

	} else {
		dev_info(dev, "Disabling Bluetooth\n");

		smba1002_wlan_bt_poweroff();
	}
	
	/* store new state */
	bt_data->powered_up = on;
}

static int bt_rfkill_set_block(void *data, bool blocked)
{
	struct device *dev = data;
	dev_dbg(dev, "blocked %d\n", blocked);

	__smba1002_pm_bt_toggle_radio(dev, !blocked);

	return 0;
}

static const struct rfkill_ops smba1002_bt_rfkill_ops = {
       .set_block = bt_rfkill_set_block,
};

static ssize_t bt_read(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	int ret = 0;
	struct smba1002_pm_bt_data *bt_data = dev_get_drvdata(dev);
	
	if (!strcmp(attr->attr.name, "power_on")) {
		ret = bt_data->powered_up;
	} else if (!strcmp(attr->attr.name, "reset")) {
		ret = !bt_data->powered_up;
	}
#ifdef CONFIG_PM
	else if (!strcmp(attr->attr.name, "keep_on_in_suspend")) {
		ret = bt_data->keep_on_in_suspend;
	}
#endif
	
	return strlcpy(buf, (!ret) ? "0\n" : "1\n", 3);
}

static ssize_t bt_write(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	unsigned long on = simple_strtoul(buf, NULL, 10);
	struct smba1002_pm_bt_data *bt_data = dev_get_drvdata(dev);

	if (!strcmp(attr->attr.name, "power_on")) {

		rfkill_set_sw_state(bt_data->rfkill, !on); /* blocked state */
		__smba1002_pm_bt_toggle_radio(dev, on);
	} else if (!strcmp(attr->attr.name, "reset")) {
		/* reset is low-active, so we need to invert */
		rfkill_set_sw_state(bt_data->rfkill, !!on); /* blocked state */
		__smba1002_pm_bt_toggle_radio(dev, !on);
		
	}
#ifdef CONFIG_PM
	else if (!strcmp(attr->attr.name, "keep_on_in_suspend")) {
		bt_data->keep_on_in_suspend = on;
	}
#endif

	return count;
}

static DEVICE_ATTR(power_on, 0666, bt_read, bt_write);
static DEVICE_ATTR(reset, 0666, bt_read, bt_write);

#ifdef CONFIG_PM
static int smba1002_bt_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct smba1002_pm_bt_data *bt_data = dev_get_drvdata(&pdev->dev);

	dev_dbg(&pdev->dev, "suspending\n");

	bt_data->pre_resume_state = bt_data->powered_up;
	/*__smba1002_pm_bt_toggle_radio(&pdev->dev, 0); */
	if (!bt_data->keep_on_in_suspend)
		__smba1002_pm_bt_toggle_radio(&pdev->dev, 0);
	else
		dev_warn(&pdev->dev, "keeping Bluetooth ON during suspend\n");

	return 0;
}

static int smba1002_bt_resume(struct platform_device *pdev)
{
	struct smba1002_pm_bt_data *bt_data = dev_get_drvdata(&pdev->dev);
	dev_dbg(&pdev->dev, "resuming\n");

	__smba1002_pm_bt_toggle_radio(&pdev->dev, bt_data->pre_resume_state);
	return 0;
}
#else
#define smba1002_bt_suspend	NULL
#define smba1002_bt_resume	NULL
#endif

static struct attribute *smba1002_bt_sysfs_entries[] = {
	&dev_attr_power_on.attr,
	&dev_attr_reset.attr,
#ifdef CONFIG_PM
	/*&dev_attr_keep_on_in_suspend.attr,*/
#endif
	NULL
};

static struct attribute_group smba1002_bt_attr_group = {
	.name	= NULL,
	.attrs	= smba1002_bt_sysfs_entries,
};

static int __init smba1002_bt_probe(struct platform_device *pdev)
{
	/* default-on */
	const int default_blocked_state = 0;

	struct rfkill *rfkill;
	struct smba1002_pm_bt_data *bt_data;
	int ret;

	dev_dbg(&pdev->dev, "starting\n");

	bt_data = kzalloc(sizeof(*bt_data), GFP_KERNEL);
	if (!bt_data) {
		dev_err(&pdev->dev, "no memory for context\n");
		return -ENOMEM;
	}
	dev_set_drvdata(&pdev->dev, bt_data);

	ret = smba1002_wlan_bt_init();
	if (ret) {
		dev_err(&pdev->dev, "unable to init wlan/bt module\n");
		kfree(bt_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return -ENODEV;
	}

	rfkill = rfkill_alloc(pdev->name, &pdev->dev, RFKILL_TYPE_BLUETOOTH,
                            &smba1002_bt_rfkill_ops, &pdev->dev);

	if (!rfkill) {
		dev_err(&pdev->dev, "Failed to allocate rfkill\n");
		smba1002_wlan_bt_deinit();
		kfree(bt_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return -ENOMEM;
	}
	bt_data->rfkill = rfkill;

	/* Set the default state */
	rfkill_init_sw_state(rfkill, default_blocked_state);
	__smba1002_pm_bt_toggle_radio(&pdev->dev, !default_blocked_state);


	ret = rfkill_register(rfkill);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register rfkill\n");
		rfkill_destroy(rfkill);
		smba1002_wlan_bt_deinit();		
		kfree(bt_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return ret;
	}

	dev_info(&pdev->dev, "Bluetooth RFKill driver registered\n");	
	
	return sysfs_create_group(&pdev->dev.kobj, &smba1002_bt_attr_group);
}

static int smba1002_bt_remove(struct platform_device *pdev)
{
	struct smba1002_pm_bt_data *bt_data = dev_get_drvdata(&pdev->dev);
	if (!bt_data)
		return 0;


	sysfs_remove_group(&pdev->dev.kobj, &smba1002_bt_attr_group);

	rfkill_unregister(bt_data->rfkill);
	rfkill_destroy(bt_data->rfkill);

	__smba1002_pm_bt_toggle_radio(&pdev->dev, 0);

	smba1002_wlan_bt_deinit();		

	kfree(bt_data);
	dev_set_drvdata(&pdev->dev, NULL);

	return 0;
}
static struct platform_driver smba1002_bt_driver_ops = {
	.probe		= smba1002_bt_probe,
	.remove		= smba1002_bt_remove,
	.suspend	= smba1002_bt_suspend,
	.resume		= smba1002_bt_resume,
	.driver		= {
		.name		= "smba1002-pm-bt",
	},
};

static int __devinit smba1002_bt_init(void)
{
	return platform_driver_register(&smba1002_bt_driver_ops);
}

static void smba1002_bt_exit(void)
{
	platform_driver_unregister(&smba1002_bt_driver_ops);
}

module_init(smba1002_bt_init);
module_exit(smba1002_bt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eduardo Jos� Tagle <ejtagle@tutopia.com>");
MODULE_DESCRIPTION("Shuttle Bluetooth power management");


 
