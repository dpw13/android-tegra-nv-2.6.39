/*
 * arch/arm/mach-tegra/include/mach/shuttle_audio.h
 *
 * Copyright 2011 Eduardo Jos� Tagle <ejtagle@tutopia.com>
 * Copyright 2011 NVIDIA, Inc.
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

struct shuttle_audio_platform_data {
	int 				  gpio_hp_det;		/* GPIO used to detect Headphone plugged in */
	int					  hifi_codec_datafmt;/* HiFi codec data format */
	bool				  hifi_codec_master;/* If Hifi codec is master */
	int					  bt_codec_datafmt;	/* Bluetooth codec data format */
	bool				  bt_codec_master;	/* If bt codec is master */
};
