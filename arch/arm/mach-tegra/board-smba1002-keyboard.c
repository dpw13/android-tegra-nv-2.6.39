/* OK */
/*
 * arch/arm/mach-tegra/board-smba1002-keyboard.c
 *
 * Copyright (C) 2011 Eduardo José Tagle <ejtagle@tutopia.com>
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

#include <linux/platform_device.h>
#include <linux/input.h>

#include <linux/gpio_keys.h>
#include <linux/gpio_shortlong_key.h>

#include <linux/gpio.h>
#include <asm/mach-types.h>

#include "board-smba1002.h"
#include "gpio-names.h"

static struct gpio_keys_button smba1002_keys[] = {
	[0] = {
		.gpio = SMBA1002_KEY_VOLUMEUP,
		.active_low = true,
		.debounce_interval = 10,
		.wakeup = false,		
		.code = KEY_VOLUMEUP,
		.type = EV_KEY,		
		.desc = "volume up",
	},
	[1] = {
		.gpio = SMBA1002_KEY_VOLUMEDOWN,
		.active_low = true,
		.debounce_interval = 10,
		.wakeup = false,		
		.code = KEY_VOLUMEDOWN,
		.type = EV_KEY,		
		.desc = "volume down",
	},
	[2] = {
		.gpio = SMBA1002_KEY_POWER,
		.active_low = true,
		.debounce_interval = 50,
		.wakeup = true,		
		.code = KEY_POWER,
		.type = EV_KEY,		
		.desc = "power",
	},
	[3] = {
		.gpio = SMBA1002_KEY_BACK,
		.active_low = true,
		.debounce_interval = 10,
		.wakeup = true,		
		.code = KEY_BACK,
		.type = EV_KEY,		
		.desc = "back",
	},
};


static struct gpio_keys_platform_data smba1002_keys_platform_data = {
	.buttons 	= smba1002_keys,
	.nbuttons 	= ARRAY_SIZE(smba1002_keys),
	.rep		= true, /* auto repeat disabled */
};

static struct platform_device smba1002_keys_device = {
	.name 		= "gpio-keys",
	.id 		= 0,
	.dev		= {
		.platform_data = &smba1002_keys_platform_data,
	},
};

static struct platform_device *smba1002_pmu_devices[] __initdata = {
	&smba1002_keys_device,
};

/* Register all keyboard devices */
int __init smba1002_keyboard_register_devices(void)
{
	return platform_add_devices(smba1002_pmu_devices, ARRAY_SIZE(smba1002_pmu_devices));
}

