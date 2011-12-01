/*
 * NVIDIA Tegra SOC - temperature sensor driver
 *
 * Copyright (C) 2011 NVIDIA Corporation
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

#if 0
#define VERBOSE_DEBUG
#define DEBUG
#endif

#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>

#include <mach/iomap.h>
#include <mach/clk.h>
#include <mach/delay.h>
#include <mach/tsensor.h>
#include <mach/tegra_fuse.h>

/* macro to enable tsensor hw reset and clock divide */
#define ENABLE_TSENSOR_HW_RESET 0

/* We have multiple tsensor instances with following registers */
#define SENSOR_CFG0				0x40
#define SENSOR_CFG1				0x48
#define SENSOR_CFG2				0x4c
#define SENSOR_STATUS0				0x58
#define SENSOR_TS_STATUS1			0x5c
#define SENSOR_TS_STATUS2			0x60

/* interrupt mask in tsensor status register */
#define TSENSOR_SENSOR_X_STATUS0_0_INTR_MASK	(1 << 8)

#define SENSOR_CFG0_M_MASK			0xffff
#define SENSOR_CFG0_M_SHIFT			8
#define SENSOR_CFG0_N_MASK			0xff
#define SENSOR_CFG0_N_SHIFT			24
#define SENSOR_CFG0_RST_INTR_SHIFT		6
#define SENSOR_CFG0_HW_DIV2_INTR_SHIFT		5
#define SENSOR_CFG0_OVERFLOW_INTR		4
#define SENSOR_CFG0_RST_ENABLE_SHIFT		2
#define SENSOR_CFG0_HW_DIV2_ENABLE_SHIFT	1
#define SENSOR_CFG0_STOP_SHIFT			0

#define SENSOR_CFG_X_TH_X_MASK			0xffff
#define SENSOR_CFG1_TH2_SHIFT			16
#define SENSOR_CFG1_TH1_SHIFT			0
#define SENSOR_CFG2_TH3_SHIFT			0
#define SENSOR_CFG2_TH0_SHIFT			16

#define SENSOR_STATUS_AVG_VALID_SHIFT		10
#define SENSOR_STATUS_CURR_VALID_SHIFT		9

#define STATE_MASK				0x7
#define STATUS0_STATE_SHIFT			0
#define STATUS0_PREV_STATE_SHIFT		4

#define LOCAL_STR_SIZE1				60
#define MAX_STR_LINE				100
#define MAX_TSENSOR_LOOP1			(1000 * 2)

#define TSENSOR_COUNTER_TOLERANCE		100

#define SENSOR_CTRL_RST_SHIFT			1
#define RST_SRC_MASK				0x7
#define RST_SRC_SENSOR				2
#define TEGRA_REV_REG_OFFSET			0x804
#define CCLK_G_BURST_POLICY_REG_REL_OFFSET	0x368
#define TSENSOR_SLOWDOWN_BIT			23

/* tsensor instance used for temperature calculation */
#define TSENSOR_FUSE_REVISION_DECIMAL_REV1	8
#define TSENSOR_FUSE_REVISION_DECIMAL_REV2	21

/* macros used for temperature calculations */
#define get_temperature_int(X)			((X) / 100)
#define get_temperature_fraction(X)		(((int)(abs(X))) % 100)
#define get_temperature_round(X)		DIV_ROUND_CLOSEST(X, 100)

/* tsensor states */
enum ts_state {
	TS_INVALID = 0,
	TS_LEVEL0,
	TS_LEVEL1,
	TS_LEVEL2,
	TS_LEVEL3,
	TS_OVERFLOW,
	TS_MAX_STATE = TS_OVERFLOW
};

/* composite type with tsensor state */
struct tsensor_state {
	unsigned int prev_state;
	unsigned int state;
};

enum {
	/* temperature is sensed from 2 points on tegra */
	TSENSOR_COUNT = 2,
	TSENSOR_INSTANCE1 = 0,
	TSENSOR_INSTANCE2 = 1,
	/* divide by 2 temperature threshold */
	DIV2_CELSIUS_TEMP_THRESHOLD_DEFAULT = 70,
	/* reset chip temperature threshold */
	RESET_CELSIUS_TEMP_THRESHOLD_DEFAULT = 75,
	/* tsensor frequency in Hz for clk src CLK_M and divisor=24 */
	DEFAULT_TSENSOR_CLK_HZ = 500000,
	DEFAULT_TSENSOR_N = 255,
	DEFAULT_TSENSOR_M = 500,
	/* tsensor instance offset */
	TSENSOR_INSTANCE_OFFSET = 0x40,
	MIN_THRESHOLD = 0x0,
	MAX_THRESHOLD = 0xffff,
	DEFAULT_THRESHOLD_TH0 = MAX_THRESHOLD,
	DEFAULT_THRESHOLD_TH1 = MAX_THRESHOLD,
	DEFAULT_THRESHOLD_TH2 = MAX_THRESHOLD,
	DEFAULT_THRESHOLD_TH3 = MAX_THRESHOLD,
};

/* constants used to implement sysfs interface */
enum tsensor_params {
	TSENSOR_PARAM_TH1 = 0,
	TSENSOR_PARAM_TH2,
	TSENSOR_PARAM_TH3,
	TSENSOR_TEMPERATURE
};

/*
 * For each registered chip, we need to keep some data in memory.
 * The structure is dynamically allocated.
 */
struct tegra_tsensor_data {
	struct device *hwmon_dev;
	spinlock_t tsensor_lock;
	struct clk *dev_clk;
	/* tsensor register space */
	void __iomem		*base;
	unsigned long		phys;
	unsigned long		phys_end;
	/* pmc register space */
	void __iomem		*pmc_rst_base;
	unsigned long		pmc_phys;
	unsigned long		pmc_phys_end;
	/* clk register space */
	void __iomem		*clk_rst_base;
	int			irq;
	unsigned int		int_status[TSENSOR_COUNT];

	/* threshold for hardware triggered clock divide by 2 */
	int div2_temp;
	/* temperature threshold for hardware triggered system reset */
	int reset_temp;
	/* temperature threshold to trigger software interrupt */
	int sw_intr_temp;
	int hysteresis;
	unsigned int ts_state_saved[TSENSOR_COUNT];
	/* save configuration before suspend and restore after resume */
	unsigned int config0[TSENSOR_COUNT];
	unsigned int config1[TSENSOR_COUNT];
	unsigned int config2[TSENSOR_COUNT];
	/* temperature readings from tsensor_index tsensor - 0/1 */
	unsigned int tsensor_index;
	int A_e_minus6;
	int B_e_minus2;
	unsigned int fuse_T1;
	unsigned int fuse_F1;
	unsigned int fuse_T2;
	unsigned int fuse_F2;
	/* Quadratic fit coefficients: m=-0.003512 n=1.528943 p=-11.1 */
	int m_e_minus6;
	int n_e_minus6;
	int p_e_minus2;
};

enum {
	TSENSOR_COEFF_SET1 = 0,
	TSENSOR_COEFF_SET2,
	TSENSOR_COEFF_END
};

struct tegra_tsensor_coeff {
	int e_minus6_m;
	int e_minus6_n;
	int e_minus2_p;
};

static struct tegra_tsensor_coeff coeff_table[] = {
	/* Quadratic fit coefficients: m=-0.002775 n=1.338811 p=-7.30 */
	[TSENSOR_COEFF_SET1] = {
		-2775,
		1338811,
		-730
	},
	/* Quadratic fit coefficients: m=-0.003512 n=1.528943 p=-11.1 */
	[TSENSOR_COEFF_SET2] = {
		-3512,
		1528943,
		-1110
	}
	/* FIXME: add tsensor coefficients after chip characterization */
};

static char my_fixed_str[LOCAL_STR_SIZE1] = "YYYYYY";
static char error_str[LOCAL_STR_SIZE1] = "ERROR:";
static unsigned int init_flag;

static int tsensor_count_2_temp(struct tegra_tsensor_data *data,
	unsigned int count, int *p_temperature);
static unsigned int tsensor_get_threshold_counter(
	struct tegra_tsensor_data *data, int temp);

/* tsensor register access functions */

static void tsensor_writel(struct tegra_tsensor_data *data, u32 val,
				unsigned long reg)
{
	unsigned int reg_offset = reg & 0xffff;
	unsigned char inst = (reg >> 16) & 0xffff;
	writel(val, data->base + (inst * TSENSOR_INSTANCE_OFFSET) +
		reg_offset);
	return;
}

static unsigned int tsensor_readl(struct tegra_tsensor_data *data,
				unsigned long reg)
{
	unsigned int reg_offset = reg & 0xffff;
	unsigned char inst = (reg >> 16) & 0xffff;
	return readl(data->base +
		(inst * TSENSOR_INSTANCE_OFFSET) + reg_offset);
}

static unsigned int tsensor_get_reg_field(
	struct tegra_tsensor_data *data, unsigned int reg,
	unsigned int shift, unsigned int mask)
{
	unsigned int reg_val;
	reg_val = tsensor_readl(data, reg);
	return (reg_val & (mask << shift)) >> shift;
}

static int tsensor_set_reg_field(
	struct tegra_tsensor_data *data, unsigned int value,
	unsigned int reg, unsigned int shift, unsigned int mask)
{
	unsigned int reg_val;
	unsigned int rd_val;
	reg_val = tsensor_readl(data, reg);
	reg_val &= ~(mask << shift);
	reg_val |= ((value & mask) << shift);
	tsensor_writel(data, reg_val, reg);
	rd_val = tsensor_readl(data, reg);
	if (rd_val == reg_val)
		return 0;
	else
		return -EINVAL;
}

/* enable argument is true to enable reset, false disables pmc reset */
static void pmc_rst_enable(struct tegra_tsensor_data *data, bool enable)
{
	unsigned int val;
	/* mapped first pmc reg is SENSOR_CTRL */
	val = readl(data->pmc_rst_base);
	if (enable)
		val |= (1 << SENSOR_CTRL_RST_SHIFT);
	else
		val &= ~(1 << SENSOR_CTRL_RST_SHIFT);
	writel(val, data->pmc_rst_base);
}

/* true returned when pmc reset source is tsensor */
static bool pmc_check_rst_sensor(struct tegra_tsensor_data *data)
{
	unsigned int val;
	unsigned char src;
	val = readl(data->pmc_rst_base + 4);
	src = (unsigned char)(val & RST_SRC_MASK);
	if (src == RST_SRC_SENSOR)
		return true;
	else
		return false;
}

/* function to get chip revision */
static void get_chip_rev(unsigned short *p_id, unsigned short *p_major,
		unsigned short *p_minor)
{
	unsigned int reg;

	reg = readl(IO_TO_VIRT(TEGRA_APB_MISC_BASE) +
		TEGRA_REV_REG_OFFSET);
	*p_id = (reg >> 8) & 0xff;
	*p_major = (reg >> 4) & 0xf;
	*p_minor = (reg >> 16) & 0xf;
	pr_info("Tegra chip revision for tsensor detected as: "
		"Chip Id=%x, Major=%d, Minor=%d\n", (int)*p_id,
		(int)*p_major, (int)*p_minor);
}

/*
 * function to get chip revision specific tsensor coefficients
 * obtained after chip characterization
 */
static void get_chip_tsensor_coeff(struct tegra_tsensor_data *data)
{
	unsigned short chip_id, major_rev, minor_rev;
	unsigned short coeff_index;

	get_chip_rev(&chip_id, &major_rev, &minor_rev);
	switch (minor_rev) {
	default:
		pr_info("Warning: tsensor coefficient for chip pending\n");
	case 1:
		coeff_index = TSENSOR_COEFF_SET1;
		break;
	}
	if (data->tsensor_index == TSENSOR_INSTANCE1)
		coeff_index = TSENSOR_COEFF_SET2;
	data->m_e_minus6 = coeff_table[coeff_index].e_minus6_m;
	data->n_e_minus6 = coeff_table[coeff_index].e_minus6_n;
	data->p_e_minus2 = coeff_table[coeff_index].e_minus2_p;
}

/* tsensor counter read function */
static unsigned int tsensor_read_counter(
	struct tegra_tsensor_data *data, u8 instance,
	unsigned int *p_counterA, unsigned int *p_counterB)
{
	unsigned int status_reg;
	unsigned int config0;
	int iter_count = 0;
	const int max_loop = 50;

	do {
		config0 = tsensor_readl(data, ((instance << 16) |
			SENSOR_CFG0));
		if (config0 & (1 << SENSOR_CFG0_STOP_SHIFT)) {
			dev_dbg(data->hwmon_dev, "Error: tsensor "
				"counter read with STOP bit not supported\n");
			*p_counterA = 0;
			*p_counterB = 0;
			return 0;
		}
		status_reg = tsensor_readl(data,
			(instance << 16) | SENSOR_STATUS0);
		if ((status_reg & (1 <<
			SENSOR_STATUS_AVG_VALID_SHIFT)) &&
			(status_reg & (1 <<
			SENSOR_STATUS_CURR_VALID_SHIFT))) {
			*p_counterA = tsensor_readl(data, (instance
				<< 16) | SENSOR_TS_STATUS1);
			*p_counterB = tsensor_readl(data, (instance
				<< 16) | SENSOR_TS_STATUS2);
			break;
		}
		if (!(iter_count % 10))
			dev_dbg(data->hwmon_dev, "retry %d\n", iter_count);
		msleep(21);
		iter_count++;
	} while (iter_count < max_loop);
	if (iter_count == max_loop)
		return -ENODEV;
	return 0;
}

/* tsensor threshold print function */
static void dump_threshold(struct tegra_tsensor_data *data)
{
	int i;
	unsigned int TH_2_1, TH_0_3;
	unsigned int curr_avg, min_max;
	int err;
	for (i = 0; i < TSENSOR_COUNT; i++) {
		TH_2_1 = tsensor_readl(data, ((i << 16) | SENSOR_CFG1));
		TH_0_3 = tsensor_readl(data, ((i << 16) | SENSOR_CFG2));
		dev_dbg(data->hwmon_dev, "Tsensor[%d]: TH_2_1=0x%x, "
			"TH_0_3=0x%x\n", i, TH_2_1, TH_0_3);
		err = tsensor_read_counter(data, i, &curr_avg, &min_max);
		if (err < 0)
			pr_err("Error: tsensor %d counter read, "
				"err=%d\n", i, err);
		else
			dev_dbg(data->hwmon_dev, "Tsensor[%d]: "
				"curr_avg=0x%x, min_max=0x%x\n",
				i, curr_avg, min_max);
	}
}

/* tsensor temperature show function */
static ssize_t tsensor_show_counters(struct device *dev,
	struct device_attribute *da, char *buf)
{
	int i;
	unsigned int curr_avg[TSENSOR_COUNT];
	unsigned int min_max[TSENSOR_COUNT];
	char err_str[] = "error-sysfs-counter-read\n";
	char fixed_str[MAX_STR_LINE];
	struct tegra_tsensor_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int err;
	int temp0, temp1;

	if (attr->index == TSENSOR_TEMPERATURE)
		snprintf(fixed_str, MAX_STR_LINE, "temperature: ");
	else
		goto error;
	for (i = 0; i < TSENSOR_COUNT; i++) {
		err = tsensor_read_counter(data, i,
			&curr_avg[i], &min_max[i]);
		if (err < 0)
			goto error;
	}
	if (attr->index == TSENSOR_TEMPERATURE) {
		/* use current counter value to calculate temperature */
		err = tsensor_count_2_temp(data,
			((curr_avg[0] & 0xFFFF0000) >> 16), &temp0);
		dev_vdbg(data->hwmon_dev, "%s has curr_avg=0x%x, "
			"minmax=0x%x, temp0=%d\n", __func__,
			curr_avg[0], min_max[0], temp0);
		if (err < 0)
			goto error;
		err = tsensor_count_2_temp(data,
			((curr_avg[1] & 0xFFFF0000) >> 16), &temp1);
		dev_vdbg(data->hwmon_dev, "%s has curr_avg=0x%x, "
			"minmax=0x%x, temp1=%d\n", __func__,
			curr_avg[1], min_max[1], temp1);
		if (err < 0)
			goto error;
		snprintf(buf, (((LOCAL_STR_SIZE1 << 1) + 3) +
			strlen(fixed_str)),
			"%s "
			"[%d]: current counter=0x%x, %d.%d"
			" deg Celsius ", fixed_str,
			data->tsensor_index,
			((curr_avg[data->tsensor_index] & 0xFFFF0000) >> 16),
			get_temperature_int(temp1),
			get_temperature_fraction(temp1));
	}
	strcat(buf, "\n");
	return strlen(buf);
error:
	return snprintf(buf, strlen(err_str),
		"%s", err_str);
}

/* utility function to check hw clock divide by 2 condition */
static bool cclkg_check_hwdiv2_sensor(struct tegra_tsensor_data *data)
{
	unsigned int val;
	val = readl(IO_ADDRESS(TEGRA_CLK_RESET_BASE +
		CCLK_G_BURST_POLICY_REG_REL_OFFSET));
	if ((1 << TSENSOR_SLOWDOWN_BIT) & val) {
		dev_err(data->hwmon_dev, "Warning: ***** tsensor "
			"slowdown bit detected\n");
		return true;
	} else {
		return false;
	}
}

/*
 * function with table to return register, field shift and mask
 * values for supported parameters
 */
static int get_param_values(
	struct tegra_tsensor_data *data, unsigned int indx,
	unsigned int *p_reg, unsigned int *p_sft, unsigned int *p_msk)
{
	switch (indx) {
	case TSENSOR_PARAM_TH1:
		*p_reg = ((data->tsensor_index << 16) | SENSOR_CFG1);
		*p_sft = SENSOR_CFG1_TH1_SHIFT;
		*p_msk = SENSOR_CFG_X_TH_X_MASK;
		snprintf(my_fixed_str, LOCAL_STR_SIZE1, "TH1[%d]: ",
			data->tsensor_index);
		break;
	case TSENSOR_PARAM_TH2:
		*p_reg = ((data->tsensor_index << 16) | SENSOR_CFG1);
		*p_sft = SENSOR_CFG1_TH2_SHIFT;
		*p_msk = SENSOR_CFG_X_TH_X_MASK;
		snprintf(my_fixed_str, LOCAL_STR_SIZE1, "TH2[%d]: ",
			data->tsensor_index);
		break;
	case TSENSOR_PARAM_TH3:
		*p_reg = ((data->tsensor_index << 16) | SENSOR_CFG2);
		*p_sft = SENSOR_CFG2_TH3_SHIFT;
		*p_msk = SENSOR_CFG_X_TH_X_MASK;
		snprintf(my_fixed_str, LOCAL_STR_SIZE1, "TH3[%d]: ",
			data->tsensor_index);
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

/* tsensor driver sysfs show function */
static ssize_t show_tsensor_param(struct device *dev,
				struct device_attribute *da,
				char *buf)
{
	unsigned int val;
	struct tegra_tsensor_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	unsigned int reg;
	unsigned int sft;
	unsigned int msk;
	int err;
	int temp;

	err = get_param_values(data, attr->index, &reg, &sft, &msk);
	if (err < 0)
		goto labelErr;
	val = tsensor_get_reg_field(data, reg, sft, msk);
	if (val == MAX_THRESHOLD)
		snprintf(buf, LOCAL_STR_SIZE1 + strlen(my_fixed_str),
			"%s un-initialized threshold ",
			my_fixed_str);
	else {
		err = tsensor_count_2_temp(data, val, &temp);
		if (err != 0)
			goto labelErr;
		snprintf(buf, LOCAL_STR_SIZE1 + strlen(my_fixed_str),
			"%s threshold: %d.%d Celsius ",
			my_fixed_str, get_temperature_int(temp),
			get_temperature_fraction(temp));
	}
	strcat(buf, "\n");
	return strlen(buf);
labelErr:
	snprintf(buf, strlen(error_str), "%s", error_str);
	return strlen(buf);
}

/* tsensor driver sysfs store function */
static ssize_t set_tsensor_param(struct device *dev,
			struct device_attribute *da,
			const char *buf, size_t count)
{
	int num;
	struct tegra_tsensor_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	unsigned int reg;
	unsigned int sft;
	unsigned int msk;
	int err;
	unsigned int counter;
	unsigned int val;

	if (strict_strtoul(buf, 0, (long int *)&num)) {
		dev_err(dev, "file: %s, line=%d return %s()\n",
			__FILE__, __LINE__, __func__);
		return -EINVAL;
	}

	counter = tsensor_get_threshold_counter(data, num);

	err = get_param_values(data, attr->index, &reg, &sft, &msk);
	if (err < 0)
		goto labelErr;

	err = tsensor_set_reg_field(data, counter, reg, sft, msk);
	if (err < 0)
		goto labelErr;

	/* TH2 clk divide check */
	if (attr->index == TSENSOR_PARAM_TH2) {
		msleep(21);
		(void)cclkg_check_hwdiv2_sensor(data);
	}
	val = tsensor_get_reg_field(data, reg, sft, msk);
	dev_dbg(dev, "%s 0x%x\n", my_fixed_str, val);
	return count;
labelErr:
	dev_err(dev, "file: %s, line=%d, %s(), error=0x%x\n", __FILE__,
		__LINE__, __func__, err);
	return 0;
}

static struct sensor_device_attribute tsensor_nodes[] = {
	SENSOR_ATTR(tsensor_TH1, S_IRUGO | S_IWUSR,
		show_tsensor_param, set_tsensor_param, TSENSOR_PARAM_TH1),
	SENSOR_ATTR(tsensor_TH2, S_IRUGO | S_IWUSR,
		show_tsensor_param, set_tsensor_param, TSENSOR_PARAM_TH2),
	SENSOR_ATTR(tsensor_TH3, S_IRUGO | S_IWUSR,
		show_tsensor_param, set_tsensor_param, TSENSOR_PARAM_TH3),
	SENSOR_ATTR(tsensor_temperature, S_IRUGO | S_IWUSR,
		tsensor_show_counters, NULL, TSENSOR_TEMPERATURE),
};

/*
 * returns current state of tsensor
 * input: tsensor instance
 *	initializes argument pointer to tsensor_state
 */
static void get_ts_state(struct tegra_tsensor_data *data,
	unsigned char inst, struct tsensor_state *p_state)
{
	p_state->prev_state =
		tsensor_get_reg_field(data,
		((inst << 16) | SENSOR_STATUS0),
		STATUS0_PREV_STATE_SHIFT, STATE_MASK);
	p_state->state =
		tsensor_get_reg_field(data,
		((inst << 16) | SENSOR_STATUS0),
		STATUS0_STATE_SHIFT, STATE_MASK);
}

/* tsensor driver interrupt handler */
static irqreturn_t tegra_tsensor_isr(int irq, void *arg_data)
{
	struct tegra_tsensor_data *data =
		(struct tegra_tsensor_data *)arg_data;
	unsigned long flags;
	unsigned int val;
	unsigned int i;
	struct tsensor_state new_state;

	spin_lock_irqsave(&data->tsensor_lock, flags);

	for (i = 0; i < TSENSOR_COUNT; i++) {
		val = tsensor_readl(data, ((i << 16) | SENSOR_STATUS0));
		tsensor_writel(data, val, ((i << 16) | SENSOR_STATUS0));
		if (val & TSENSOR_SENSOR_X_STATUS0_0_INTR_MASK) {
			dev_err(data->hwmon_dev, "tsensor instance-%d "
				"interrupt\n", i);
			get_ts_state(data, (unsigned char)i, &new_state);
			/* counter overflow check */
			if (new_state.state == TS_OVERFLOW)
				dev_err(data->hwmon_dev, "Warning: "
					"***** OVERFLOW tsensor\n");
			if (new_state.state != data->ts_state_saved[i]) {
				dev_err(data->hwmon_dev, "TS state "
					"change: old=%d, new=%d\n",
					data->ts_state_saved[i],
					new_state.state);
				data->ts_state_saved[i] = new_state.state;
			}
		}
	}

	spin_unlock_irqrestore(&data->tsensor_lock, flags);

	return IRQ_HANDLED;
}

/*
 * function to read fuse registers and give - T1, T2, F1 and F2
 */
static int read_tsensor_fuse_regs(struct tegra_tsensor_data *data)
{
	unsigned int reg1;
	unsigned int T1 = 0, T2 = 0;
	unsigned int spare_bits;
	int err;

	/* read tsensor calibration register */
	/*
	 * High (~90 DegC) Temperature Calibration value (upper 16 bits of
	 *          FUSE_TSENSOR_CALIB_0) - F2
	 * Low (~25 deg C) Temperature Calibration value (lower 16 bits of
	 *          FUSE_TSENSOR_CALIB_0) - F1
	 */
	err = tegra_fuse_get_tsensor_calibration_data(&reg1);
	if (err)
		goto errLabel;
	data->fuse_F1 = reg1 & 0xFFFF;
	data->fuse_F2 = (reg1 >> 16) & 0xFFFF;

	err = tegra_fuse_get_tsensor_spare_bits(&spare_bits);
	if (err) {
		pr_err("tsensor spare bit fuse read error=%d\n", err);
		goto errLabel;
	}

	/*
	 * FUSE_TJ_ADT_LOWT = T1, FUSE_TJ_ADJ = T2
	 */

	/*
	 * Low temp is:
	 * FUSE_TJ_ADT_LOWT = bits [20:14] or’ed with bits [27:21]
	 */
	T1 = ((spare_bits >> 14) & 0x7F) |
		((spare_bits >> 21) & 0x7F);
	dev_vdbg(data->hwmon_dev, "Tsensor low temp (T1) fuse :\n");

	/*
	 * High temp is:
	 * FUSE_TJ_ADJ = bits [6:0] or’ed with bits [13:7]
	 */
	dev_vdbg(data->hwmon_dev, "Tsensor low temp (T2) fuse :\n");
	T2 = (spare_bits & 0x7F) | ((spare_bits >> 7) & 0x7F);
	pr_info("Tsensor fuse calibration F1=%d, F2=%d, T1=%d, T2=%d\n"
		, data->fuse_F1, data->fuse_F2, T1, T2);
	data->fuse_T1 = T1;
	data->fuse_T2 = T2;
	return 0;
errLabel:
	return err;
}

/* function to calculate interim temperature */
static int calc_interim_temp(struct tegra_tsensor_data *data,
	unsigned int counter, int *p_interim_temp)
{
	int val1;
	/*
	 * T-int = A * Counter + B
	 * (Counter is the sensor frequency output)
	 */
	if ((data->fuse_F2 - data->fuse_F1) <= (data->fuse_T2 -
		data->fuse_T1)) {
		dev_err(data->hwmon_dev, "Error: F2=%d, F1=%d "
			"difference unexpectedly low. "
			"Aborting temperature processing\n", data->fuse_F2,
			data->fuse_F1);
		return -EINVAL;
	} else {
		/* expression modified after assuming s_A is 10^6 times,
		 * s_B is 10^2 times and want end result to be 10^2 times
		 * actual value
		 */
		val1 = DIV_ROUND_CLOSEST((data->A_e_minus6 * counter) , 10000);
		dev_vdbg(data->hwmon_dev, "A*counter / 100 = %d\n",
			val1);
		*p_interim_temp = (val1 + data->B_e_minus2);
	}
	dev_dbg(data->hwmon_dev, "tsensor: counter=0x%x, interim "
		"temp*100=%d\n",
		counter, *p_interim_temp);
	return 0;
}

/*
 * function to calculate final temperature, given
 * interim temperature
 */
static void calc_final_temp(struct tegra_tsensor_data *data,
	int interim_temp, int *p_final_temp)
{
	int temp1, temp2, temp;
	/*
	 * T-final = m * T-int ^2 + n * T-int + p
	 * m = -0.002775
	 * n = 1.338811
	 * p = -7.3
	 */

	dev_vdbg(data->hwmon_dev, "interim_temp=%d\n", interim_temp);
	temp1 = (DIV_ROUND_CLOSEST((interim_temp * interim_temp) , 100));
	dev_vdbg(data->hwmon_dev, "temp1=%d\n", temp1);
	temp1 *= (DIV_ROUND_CLOSEST(data->m_e_minus6 , 10));
	dev_vdbg(data->hwmon_dev, "m*T-int^2=%d\n", temp1);
	temp1 = (DIV_ROUND_CLOSEST(temp1, 10000));
	/* we want to keep 3 decimal point digits */
	dev_vdbg(data->hwmon_dev, "m*T-int^2 / 10000=%d\n", temp1);
	dev_dbg(data->hwmon_dev, "temp1*100=%d\n", temp1);

	temp2 = (DIV_ROUND_CLOSEST(interim_temp * (
		DIV_ROUND_CLOSEST(data->n_e_minus6, 100)
		), 1000)); /* 1000 times actual */
	dev_vdbg(data->hwmon_dev, "n*T-int =%d\n", temp2);

	temp = temp1 + temp2;
	dev_vdbg(data->hwmon_dev, "m*T-int^2 + n*T-int =%d\n", temp);
	temp += (data->p_e_minus2 * 10);
	temp = DIV_ROUND_CLOSEST(temp, 10);
	/* final temperature(temp) is 100 times actual value
	 * to preserve 2 decimal digits and enable fixed point
	 * computation
	 */
	dev_vdbg(data->hwmon_dev, "m*T-int^2 + n*T-int + p =%d\n",
		temp);
	dev_dbg(data->hwmon_dev, "Final temp=%d.%d\n",
		get_temperature_int(temp), get_temperature_fraction(temp));
	*p_final_temp = (int)(temp);
}

/*
 * Function to compute constants A and B needed for temperature
 * calculation
 * A = (T2-T1) / (F2-F1)
 * B = T1 – A * F1
 */
static int tsensor_get_const_AB(struct tegra_tsensor_data *data)
{
	int err;

	/*
	 *   1. Find fusing registers for 25C (T1, F1) and 90C (T2, F2);
	 */
	err = read_tsensor_fuse_regs(data);
	if (err) {
		dev_err(data->hwmon_dev, "Fuse register read required "
			"for internal tsensor returns err=%d\n", err);
		return err;
	}

	if (data->fuse_F2 != data->fuse_F1) {
		if ((data->fuse_F2 - data->fuse_F1) <= (data->fuse_T2 -
			data->fuse_T1)) {
			dev_err(data->hwmon_dev, "Error: F2=%d, "
				"F1=%d, difference"
				" unexpectedly low. Aborting temperature"
				"computation\n", data->fuse_F2, data->fuse_F1);
			return -EINVAL;
		} else {
			data->A_e_minus6 = ((data->fuse_T2 - data->fuse_T1) *
				1000000);
			data->A_e_minus6 /= (data->fuse_F2 - data->fuse_F1);
			data->B_e_minus2 = (data->fuse_T1 * 100) - (
				DIV_ROUND_CLOSEST((data->A_e_minus6 *
				data->fuse_F1), 10000));
			/* B is 100 times now */
		}
	}
	dev_dbg(data->hwmon_dev, "A_e_minus6 = %d\n", data->A_e_minus6);
	dev_dbg(data->hwmon_dev, "B_e_minus2 = %d\n", data->B_e_minus2);
	return 0;
}

/*
 * function calculates expected temperature corresponding to
 * given tsensor counter value
 * Value returned is 100 times calculated temperature since the
 * calculations are using fixed point arithmetic instead of floating point
 */
static int tsensor_count_2_temp(struct tegra_tsensor_data *data,
	unsigned int count, int *p_temperature)
{
	int interim_temp;
	int err;

	/*
	 *
	 * 2. Calculate interim temperature:
	 */
	err = calc_interim_temp(data, count, &interim_temp);
	if (err < 0) {
		dev_err(data->hwmon_dev, "tsensor: cannot read temperature\n");
		*p_temperature = -1;
		return err;
	}

	/*
	 *
	 * 3. Calculate final temperature:
	 */
	calc_final_temp(data, interim_temp, p_temperature);
	return 0;
}

/*
 * utility function implements ceil to power of 10 -
 * e.g. given 987 it returns 1000
 */
static int my_ceil_pow10(int num)
{
	int tmp;
	int val = 1;
	tmp = (num < 0) ? -num : num;
	if (tmp == 0)
		return 0;
	while (tmp > 1) {
		val *= 10;
		tmp /= 10;
	}
	return val;
}

/*
 * function to solve quadratic roots of equation
 * used to get counter corresponding to given temperature
 */
static void get_quadratic_roots(struct tegra_tsensor_data *data,
		unsigned int temp, unsigned int *p_counter1,
		unsigned int *p_counter2)
{
	/* expr1 = 2 * m * B + n */
	int expr1_e_minus6;
	/* expr2 = expr1^2 */
	int expr2_e_minus6;
	/* expr3 = m * B^2 + n * B + p */
	int expr3_e_minus4_1;
	int expr3_e_minus4_2;
	int expr3_e_minus4;
	int expr4_e_minus6;
	int expr4_e_minus2_1;
	int expr4_e_minus6_2;
	int expr4_e_minus6_3;
	int expr5_e_minus6, expr5_e_minus6_1, expr6, expr7;
	int expr8_e_minus6, expr9_e_minus6;
	int multiplier;
	const int multiplier2 = 1000000;
	int expr10_e_minus6, expr11_e_minus6;
	int expr12, expr13;

	dev_vdbg(data->hwmon_dev, "A_e_minus6=%d, B_e_minus2=%d, "
		"m_e_minus6=%d, n_e_minus6=%d, p_e_minus2=%d, "
		"temp=%d\n", data->A_e_minus6, data->B_e_minus2,
		data->m_e_minus6,
		data->n_e_minus6, data->p_e_minus2, (int)temp);
	expr1_e_minus6 = (DIV_ROUND_CLOSEST((2 * data->m_e_minus6 *
		data->B_e_minus2), 100) + data->n_e_minus6);
	dev_vdbg(data->hwmon_dev, "2_m_B_plun_e_minus6=%d\n",
		expr1_e_minus6);
	expr2_e_minus6 = (DIV_ROUND_CLOSEST(expr1_e_minus6, 1000)) *
		(DIV_ROUND_CLOSEST(expr1_e_minus6, 1000));
	dev_vdbg(data->hwmon_dev, "expr1^2=%d\n", expr2_e_minus6);
	expr3_e_minus4_1 = (DIV_ROUND_CLOSEST((
		(DIV_ROUND_CLOSEST((data->m_e_minus6 * data->B_e_minus2),
		1000)) * (DIV_ROUND_CLOSEST(data->B_e_minus2, 10))), 100));
	dev_vdbg(data->hwmon_dev, "expr3_e_minus4_1=%d\n",
		expr3_e_minus4_1);
	expr3_e_minus4_2 = DIV_ROUND_CLOSEST(
		(DIV_ROUND_CLOSEST(data->n_e_minus6, 100) * data->B_e_minus2),
		100);
	dev_vdbg(data->hwmon_dev, "expr3_e_minus4_2=%d\n",
		expr3_e_minus4_2);
	expr3_e_minus4 = expr3_e_minus4_1 + expr3_e_minus4_2;
	dev_vdbg(data->hwmon_dev, "expr3=%d\n", expr3_e_minus4);
	expr4_e_minus2_1 = DIV_ROUND_CLOSEST((expr3_e_minus4 +
		(data->p_e_minus2 * 100)), 100);
	dev_vdbg(data->hwmon_dev, "expr4_e_minus2_1=%d\n",
		expr4_e_minus2_1);
	expr4_e_minus6_2 = (4 * data->m_e_minus6);
	dev_vdbg(data->hwmon_dev, "expr4_e_minus6_2=%d\n",
		expr4_e_minus6_2);
	expr4_e_minus6 = DIV_ROUND_CLOSEST((expr4_e_minus2_1 *
		expr4_e_minus6_2), 100);
	dev_vdbg(data->hwmon_dev, "expr4_minus6=%d\n", expr4_e_minus6);
	expr5_e_minus6_1 = expr2_e_minus6 - expr4_e_minus6;
	dev_vdbg(data->hwmon_dev, "expr5_e_minus6_1=%d\n",
		expr5_e_minus6_1);
	expr4_e_minus6_3 = (expr4_e_minus6_2 * temp);
	dev_vdbg(data->hwmon_dev, "expr4_e_minus6_3=%d\n",
		expr4_e_minus6_3);
	expr5_e_minus6 = (expr5_e_minus6_1 + expr4_e_minus6_3);
	dev_vdbg(data->hwmon_dev, "expr5_e_minus6=%d\n",
		expr5_e_minus6);
	multiplier = my_ceil_pow10(expr5_e_minus6);
	dev_vdbg(data->hwmon_dev, "multiplier=%d\n", multiplier);
	expr6 = int_sqrt(expr5_e_minus6);
	dev_vdbg(data->hwmon_dev, "sqrt top=%d\n", expr6);
	expr7 = int_sqrt(multiplier);
	dev_vdbg(data->hwmon_dev, "sqrt bot=%d\n", expr7);
	if (expr7 == 0) {
		pr_err("Error: %s line=%d, expr7=%d\n",
			__func__, __LINE__, expr7);
		return;
	} else {
		expr8_e_minus6 = (expr6 * multiplier2) / expr7;
	}
	dev_vdbg(data->hwmon_dev, "sqrt final=%d\n", expr8_e_minus6);
	dev_vdbg(data->hwmon_dev, "2_m_B_plus_n_e_minus6=%d\n",
		expr1_e_minus6);
	expr9_e_minus6 = DIV_ROUND_CLOSEST((2 * data->m_e_minus6 *
		data->A_e_minus6), 1000000);
	dev_vdbg(data->hwmon_dev, "denominator=%d\n", expr9_e_minus6);
	if (expr9_e_minus6 == 0) {
		pr_err("Error: %s line=%d, expr9_e_minus6=%d\n",
			__func__, __LINE__, expr9_e_minus6);
		return;
	}
	expr10_e_minus6 = -expr1_e_minus6 - expr8_e_minus6;
	dev_vdbg(data->hwmon_dev, "expr10_e_minus6=%d\n",
		expr10_e_minus6);
	expr11_e_minus6 = -expr1_e_minus6 + expr8_e_minus6;
	dev_vdbg(data->hwmon_dev, "expr11_e_minus6=%d\n",
		expr11_e_minus6);
	expr12 = (expr10_e_minus6 / expr9_e_minus6);
	dev_vdbg(data->hwmon_dev, "counter1=%d\n", expr12);
	expr13 = (expr11_e_minus6 / expr9_e_minus6);
	dev_vdbg(data->hwmon_dev, "counter2=%d\n", expr13);
	*p_counter1 = expr12;
	*p_counter2 = expr13;
}

/*
 * function returns tsensor expected counter corresponding to input
 * temperature in degree Celsius.
 * e.g. for temperature of 35C, temp=35
 */
static void tsensor_temp_2_count(struct tegra_tsensor_data *data,
				int temp,
				unsigned int *p_counter1,
				unsigned int *p_counter2)
{
	if (temp > 0) {
		dev_dbg(data->hwmon_dev, "Trying to calculate counter"
			" for requested temperature"
			" threshold=%d\n", temp);
		/*
		 * calculate the constants needed to get roots of
		 * following quadratic eqn:
		 * m * A^2 * Counter^2 +
		 * A * (2 * m * B + n) * Counter +
		 * (m * B^2 + n * B + p - Temperature) = 0
		 */
		get_quadratic_roots(data, temp, p_counter1, p_counter2);
		/*
		 * checked at current temperature=35 the counter=11418
		 * for 50 deg temperature: counter1=22731, counter2=11817
		 * at 35 deg temperature: counter1=23137, counter2=11411
		 * hence, for above values we are assuming counter2 has
		 * the correct value
		 */
	} else {
		if (temp == data->div2_temp) {
			*p_counter1 = DEFAULT_THRESHOLD_TH2;
			*p_counter2 = DEFAULT_THRESHOLD_TH2;
		} else {
			*p_counter1 = DEFAULT_THRESHOLD_TH3;
			*p_counter2 = DEFAULT_THRESHOLD_TH3;
		}
	}
}

/*
 * function to compare computed and expected values with
 * certain tolerance setting hard coded here
 */
static bool cmp_counter(
	struct tegra_tsensor_data *data,
	unsigned int actual, unsigned int exp)
{
	unsigned int smaller;
	unsigned int larger;
	smaller = (actual > exp) ? exp : actual;
	larger = (smaller == actual) ? exp : actual;
	if ((larger - smaller) > TSENSOR_COUNTER_TOLERANCE) {
		dev_dbg(data->hwmon_dev, "actual=%d, exp=%d, larger=%d, "
		"smaller=%d, tolerance=%d\n", actual, exp, larger, smaller,
		TSENSOR_COUNTER_TOLERANCE);
		return false;
	}
	return true;
}

/* function to print chart of temperature to counter values */
static void print_temperature_2_counter_table(
	struct tegra_tsensor_data *data)
{
	int i;
	/* static list of temperature tested */
	unsigned int temp_list[] = {
		30,
		35,
		40,
		45,
		50,
		55,
		60,
		61,
		62,
		63,
		64,
		65,
		70,
		75,
		80,
		85,
		90,
		95,
		100,
		105,
		110,
		115,
		120
	};
	unsigned int counter1, counter2;
	dev_dbg(data->hwmon_dev, "Temperature and counter1 and "
		"counter2 chart **********\n");
	for (i = 0; i < ARRAY_SIZE(temp_list); i++) {
		tsensor_temp_2_count(data, temp_list[i],
			&counter1, &counter2);
		dev_dbg(data->hwmon_dev, "temperature[%d]=%d, "
			"counter1=0x%x, counter2=0x%x\n",
			i, temp_list[i], counter1, counter2);
	}
	dev_dbg(data->hwmon_dev, "\n\n");
}

static void dump_a_tsensor_reg(struct tegra_tsensor_data *data,
	unsigned int addr)
{
	dev_dbg(data->hwmon_dev, "tsensor[%d][0x%x]: 0x%x\n", (addr >> 16),
		addr & 0xFFFF, tsensor_readl(data, addr));
}

static void dump_tsensor_regs(struct tegra_tsensor_data *data)
{
	int i;
	for (i = 0; i < TSENSOR_COUNT; i++) {
		/* if STOP bit is set skip this check */
		dump_a_tsensor_reg(data, ((i << 16) | SENSOR_CFG0));
		dump_a_tsensor_reg(data, ((i << 16) | SENSOR_CFG1));
		dump_a_tsensor_reg(data, ((i << 16) | SENSOR_CFG2));
		dump_a_tsensor_reg(data, ((i << 16) | SENSOR_STATUS0));
		dump_a_tsensor_reg(data, ((i << 16) | SENSOR_TS_STATUS1));
		dump_a_tsensor_reg(data, ((i << 16) | SENSOR_TS_STATUS2));
		dump_a_tsensor_reg(data, ((i << 16) | 0x0));
		dump_a_tsensor_reg(data, ((i << 16) | 0x44));
		dump_a_tsensor_reg(data, ((i << 16) | 0x50));
		dump_a_tsensor_reg(data, ((i << 16) | 0x54));
		dump_a_tsensor_reg(data, ((i << 16) | 0x64));
		dump_a_tsensor_reg(data, ((i << 16) | 0x68));
	}
}

/*
 * function to test if conversion of counter to temperature
 * and vice-versa is working
 */
static int test_temperature_algo(struct tegra_tsensor_data *data)
{
	unsigned int actual_counter;
	unsigned int curr_avg, min_max;
	unsigned int counter1, counter2;
	unsigned int T1;
	int err = 0;
	bool result1, result2;
	bool result = false;

	/* read actual counter */
	err = tsensor_read_counter(data, data->tsensor_index, &curr_avg,
		&min_max);
	if (err < 0) {
		pr_err("Error: tsensor0 counter read, err=%d\n", err);
		goto endLabel;
	}
	actual_counter = ((curr_avg & 0xFFFF0000) >> 16);
	dev_dbg(data->hwmon_dev, "counter read=0x%x\n", actual_counter);

	/* calculate temperature */
	err = tsensor_count_2_temp(data, actual_counter, &T1);
	dev_dbg(data->hwmon_dev, "%s actual counter=0x%x, calculated "
		"temperature=%d.%d\n", __func__,
		actual_counter, get_temperature_int(T1),
		get_temperature_fraction(T1));
	if (err < 0) {
		pr_err("Error: calculate temperature step\n");
		goto endLabel;
	}

	/* calculate counter corresponding to read temperature */
	tsensor_temp_2_count(data, get_temperature_round(T1),
		&counter1, &counter2);
	dev_dbg(data->hwmon_dev, "given temperature=%d, counter1=0x%x,"
		" counter2=0x%x\n",
		get_temperature_round(T1), counter1, counter2);

	/* compare counter calculated with actual original counter */
	result1 = cmp_counter(data, actual_counter, counter1);
	result2 = cmp_counter(data, actual_counter, counter2);
	if (result1) {
		dev_dbg(data->hwmon_dev, "counter1 matches: actual=%d,"
			" calc=%d\n", actual_counter, counter1);
		result = true;
	}
	if (result2) {
		dev_dbg(data->hwmon_dev, "counter2 matches: actual=%d,"
			" calc=%d\n", actual_counter, counter2);
		result = true;
	}
	if (!result) {
		pr_info("NO Match: actual=%d,"
			" calc counter2=%d, counter1=%d\n", actual_counter,
			counter2, counter1);
		err = -EIO;
	}

endLabel:
	return err;
}

/* tsensor threshold temperature to threshold counter conversion function */
static unsigned int tsensor_get_threshold_counter(
	struct tegra_tsensor_data *data,
	int temp_threshold)
{
	unsigned int counter1, counter2;
	unsigned int curr_avg, min_max;
	unsigned int counter;
	int err;

	if (temp_threshold < 0)
		return MAX_THRESHOLD;
	tsensor_temp_2_count(data, temp_threshold, &counter1, &counter2);
	err = tsensor_read_counter(data, data->tsensor_index, &curr_avg,
		&min_max);
	if (err < 0) {
		pr_err("Error: tsensor0 counter read, err=%d\n", err);
		return MAX_THRESHOLD;
	}
	if (counter2 > ((curr_avg & 0xFFFF0000) >> 16)) {
		dev_dbg(data->hwmon_dev, "choosing counter 2=0x%x as "
			"root\n", counter2);
	} else {
		pr_err("Warning: tsensor choosing counter 2=0x%x as "
			"root, counter 1=0x%x, counter=0x%x\n", counter2,
			counter1, ((curr_avg & 0xFFFF0000) >> 16));
	}
	counter = counter2;
	return counter;
}

/* tsensor temperature threshold setup function */
static void tsensor_threshold_setup(
		struct tegra_tsensor_data *data,
		unsigned char index, bool is_default_threshold)
{
	unsigned long config0;
	unsigned char i = index;
	unsigned int th2_count = DEFAULT_THRESHOLD_TH2;
	unsigned int th3_count = DEFAULT_THRESHOLD_TH3;
	unsigned int th1_count = DEFAULT_THRESHOLD_TH1;
	unsigned int hysteresis_count;
	int th0_diff = (DEFAULT_THRESHOLD_TH1 - MIN_THRESHOLD);

	dev_dbg(data->hwmon_dev, "started tsensor_threshold_setup %d\n",
		index);
	config0 = tsensor_readl(data, ((i << 16) | SENSOR_CFG0));

	/* Choose thresholds for sensor0 and sensor1 */
	/* set to very high values initially - DEFAULT_THRESHOLD */
	if ((!is_default_threshold) && (i == data->tsensor_index)) {
		dev_dbg(data->hwmon_dev, "before div2 temp_2_count\n");
		th2_count = tsensor_get_threshold_counter(data,
			data->div2_temp);
		dev_dbg(data->hwmon_dev, "div2_temp=%d, count=%d\n",
			data->div2_temp, th2_count);
		dev_dbg(data->hwmon_dev, "before reset temp_2_count\n");
		th3_count = tsensor_get_threshold_counter(data,
			data->reset_temp);
		dev_dbg(data->hwmon_dev, "reset_temp=%d, count=%d\n",
			(unsigned int)data->reset_temp, th3_count);
		dev_dbg(data->hwmon_dev, "before sw_intr temp_2_count\n");
		th1_count = tsensor_get_threshold_counter(data,
			data->sw_intr_temp);
		dev_dbg(data->hwmon_dev, "sw_intr_temp=%d, count=%d\n",
			(unsigned int)data->sw_intr_temp, th1_count);
		dev_dbg(data->hwmon_dev, "before hysteresis temp_2_count\n");
		hysteresis_count = tsensor_get_threshold_counter(data,
			(data->sw_intr_temp - data->hysteresis));
		dev_dbg(data->hwmon_dev, "hysteresis_temp=%d, count=%d\n",
			(unsigned int)(data->sw_intr_temp - data->hysteresis),
			hysteresis_count);
		th0_diff = (th1_count == DEFAULT_THRESHOLD_TH1) ?
			DEFAULT_THRESHOLD_TH0 :
			(th1_count - hysteresis_count);
		dev_dbg(data->hwmon_dev, "th0_diff=%d\n", th0_diff);
	}
	dev_dbg(data->hwmon_dev, "before threshold program TH dump:\n");
	dump_threshold(data);
	dev_dbg(data->hwmon_dev, "th3=0x%x, th2=0x%x, th1=0x%x, th0=0x%x\n",
		th3_count, th2_count, th1_count, th0_diff);
	config0 = (((th2_count & SENSOR_CFG_X_TH_X_MASK)
		<< SENSOR_CFG1_TH2_SHIFT) |
		((th1_count & SENSOR_CFG_X_TH_X_MASK) <<
		SENSOR_CFG1_TH1_SHIFT));
	tsensor_writel(data, config0, ((i << 16) | SENSOR_CFG1));
	config0 = (((th0_diff & SENSOR_CFG_X_TH_X_MASK)
		<< SENSOR_CFG2_TH0_SHIFT) |
		((th3_count & SENSOR_CFG_X_TH_X_MASK) <<
		SENSOR_CFG2_TH3_SHIFT));
	tsensor_writel(data, config0, ((i << 16) | SENSOR_CFG2));
	dev_dbg(data->hwmon_dev, "after threshold program TH dump:\n");
	dump_threshold(data);
}

/* tsensor config programming function */
static int tsensor_config_setup(struct tegra_tsensor_data *data)
{
	unsigned int config0;
	unsigned int i;
	unsigned int status_reg;
	unsigned int no_resp_count;
	int err = 0;
	struct tsensor_state curr_state;

	for (i = 0; i < TSENSOR_COUNT; i++) {
		/*
		 * Pre-read setup:
		 * Set M and N values
		 * Enable HW features HW_FREQ_DIV_EN, THERMAL_RST_EN
		 */
		config0 = tsensor_readl(data, ((i << 16) | SENSOR_CFG0));
		config0 &= ~((SENSOR_CFG0_M_MASK << SENSOR_CFG0_M_SHIFT) |
			(SENSOR_CFG0_N_MASK << SENSOR_CFG0_N_SHIFT) |
			(1 << SENSOR_CFG0_OVERFLOW_INTR) |
			(1 << SENSOR_CFG0_RST_INTR_SHIFT) |
			(1 << SENSOR_CFG0_HW_DIV2_INTR_SHIFT) |
			(1 << SENSOR_CFG0_RST_ENABLE_SHIFT) |
			(1 << SENSOR_CFG0_HW_DIV2_ENABLE_SHIFT)
			);
		/* Set STOP bit */
		/* Set M and N values */
		/* Enable HW features HW_FREQ_DIV_EN, THERMAL_RST_EN */
		config0 |= (((DEFAULT_TSENSOR_M & SENSOR_CFG0_M_MASK) <<
			SENSOR_CFG0_M_SHIFT) |
			((DEFAULT_TSENSOR_N & SENSOR_CFG0_N_MASK) <<
			SENSOR_CFG0_N_SHIFT) |
#if ENABLE_TSENSOR_HW_RESET
			(1 << SENSOR_CFG0_OVERFLOW_INTR) |
			(1 << SENSOR_CFG0_RST_ENABLE_SHIFT) |
			(1 << SENSOR_CFG0_HW_DIV2_ENABLE_SHIFT) |
#endif
			(1 << SENSOR_CFG0_STOP_SHIFT));

		tsensor_writel(data, config0, ((i << 16) | SENSOR_CFG0));
		tsensor_threshold_setup(data, i, true);
	}

	for (i = 0; i < TSENSOR_COUNT; i++) {
		config0 = tsensor_readl(data, ((i << 16) | SENSOR_CFG0));
		/* Enables interrupts and clears sensor stop */
		/*
		 * Interrupts not enabled as software handling is not
		 * needed in rev1 driver
		 */
		/* Disable sensor stop bit */
		config0 &= ~(1 << SENSOR_CFG0_STOP_SHIFT);
		tsensor_writel(data, config0, ((i << 16) | SENSOR_CFG0));
	}

	/* Check if counters are getting updated */
	no_resp_count = 0;

	for (i = 0; i < TSENSOR_COUNT; i++) {
		/* if STOP bit is set skip this check */
		config0 = tsensor_readl(data, ((i << 16) | SENSOR_CFG0));
		if (!(config0 & (1 << SENSOR_CFG0_STOP_SHIFT))) {
			unsigned int loop_count = 0;
			do {
				status_reg = tsensor_readl(data,
					(i << 16) | SENSOR_STATUS0);
				if ((status_reg & (1 <<
					SENSOR_STATUS_AVG_VALID_SHIFT)) &&
					(status_reg & (1 <<
					SENSOR_STATUS_CURR_VALID_SHIFT))) {
					msleep(21);
					loop_count++;
					if (!(loop_count % 200))
						dev_err(data->hwmon_dev,
						" Warning: Tsensor Counter "
						"sensor%d not Valid yet.\n", i);
					if (loop_count > MAX_TSENSOR_LOOP1) {
						no_resp_count++;
						break;
					}
				}
			} while (!(status_reg &
				(1 << SENSOR_STATUS_AVG_VALID_SHIFT)) ||
				(!(status_reg &
				(1 << SENSOR_STATUS_CURR_VALID_SHIFT))));
			if (no_resp_count == TSENSOR_COUNT) {
				err = -ENODEV;
				goto skip_all;
			}
		}
		/* check initial state */
		get_ts_state(data, (unsigned char)i, &curr_state);
		data->ts_state_saved[i] = curr_state.state;
	}
	/* initialize tsensor chip coefficients */
	get_chip_tsensor_coeff(data);
skip_all:
	return err;
}

/* function to enable tsensor clock */
static int tsensor_clk_enable(
	struct tegra_tsensor_data *data,
	bool enable)
{
	int err = 0;
	unsigned long rate;
	struct clk *clk_m;

	if (enable) {
		clk_enable(data->dev_clk);
		rate = clk_get_rate(data->dev_clk);
		clk_m = clk_get_sys(NULL, "clk_m");
		if (clk_get_parent(data->dev_clk) != clk_m) {
			err = clk_set_parent(data->dev_clk, clk_m);
			if (err < 0)
				goto fail;
		}
		rate = DEFAULT_TSENSOR_CLK_HZ;
		if (rate != clk_get_rate(clk_m)) {
			err = clk_set_rate(data->dev_clk, rate);
			if (err < 0)
				goto fail;
		}
	} else {
		clk_disable(data->dev_clk);
		clk_put(data->dev_clk);
	}
fail:
	return err;
}

/*
 * This function enables the tsensor using default configuration
 * 1. We would need some configuration APIs to calibrate
 *    the tsensor counters to right temperature
 * 2. hardware triggered divide cpu clock by 2 as well pmu reset is enabled
 *    implementation. No software actions are enabled at this point
 */
static int tegra_tsensor_setup(struct platform_device *pdev)
{
	struct tegra_tsensor_data *data = platform_get_drvdata(pdev);
	struct resource *r;
	int err = 0;
	struct tegra_tsensor_platform_data *tsensor_data;

	data->dev_clk = clk_get(&pdev->dev, NULL);
	if ((!data->dev_clk) || ((int)data->dev_clk == -(ENOENT))) {
		dev_err(&pdev->dev, "Couldn't get the clock\n");
		err = PTR_ERR(data->dev_clk);
		goto fail;
	}

	/* Enable tsensor clock */
	err = tsensor_clk_enable(data, true);
	if (err < 0)
		goto err_irq;

	/* Reset tsensor */
	dev_dbg(&pdev->dev, "before tsensor reset %s\n", __func__);
	tegra_periph_reset_assert(data->dev_clk);
	udelay(100);
	tegra_periph_reset_deassert(data->dev_clk);
	udelay(100);

	dev_dbg(&pdev->dev, "before tsensor chk pmc reset %s\n",
		__func__);
	/* Check for previous resets in pmc */
	if (pmc_check_rst_sensor(data)) {
		dev_err(data->hwmon_dev, "Warning: ***** Last PMC "
			"Reset source: tsensor detected\n");
	}

	dev_dbg(&pdev->dev, "before tsensor pmc reset enable %s\n",
		__func__);
	/* Enable the sensor reset in PMC */
	pmc_rst_enable(data, true);

	/* register interrupt */
	r = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!r) {
		dev_err(&pdev->dev, "Failed to get IRQ\n");
		err = -ENXIO;
		goto err_irq;
	}
	data->irq = r->start;
	err = request_irq(data->irq, tegra_tsensor_isr,
			IRQF_DISABLED, pdev->name, data);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed to register IRQ\n");
		goto err_irq;
	}

	/* tsensor thresholds are read from board/platform specific files */
	dev_dbg(&pdev->dev, "before tsensor get platform data %s\n",
		__func__);
	tsensor_data = pdev->dev.platform_data;
#if !ENABLE_TSENSOR_HW_RESET
	/* FIXME: remove this once tsensor temperature is reliable */
	tsensor_data->hw_clk_div_temperature = -1;
	tsensor_data->hw_reset_temperature = -1;
	tsensor_data->sw_intr_temperature = -1;
	tsensor_data->hysteresis = -1;
#endif
	dev_dbg(&pdev->dev, "tsensor platform_data=0x%x\n",
		(unsigned int)pdev->dev.platform_data);
	dev_dbg(&pdev->dev, "clk_div temperature=%d\n",
		tsensor_data->hw_clk_div_temperature);
	data->div2_temp = tsensor_data->hw_clk_div_temperature;
	dev_dbg(&pdev->dev, "reset temperature=%d\n",
		tsensor_data->hw_reset_temperature);
	data->reset_temp = tsensor_data->hw_reset_temperature;
	dev_dbg(&pdev->dev, "sw_intr temperature=%d\n",
		tsensor_data->sw_intr_temperature);
	data->sw_intr_temp = tsensor_data->sw_intr_temperature;
	dev_dbg(&pdev->dev, "hysteresis temperature=%d\n",
		tsensor_data->hysteresis);
	data->hysteresis = tsensor_data->hysteresis;

	dev_dbg(&pdev->dev, "before tsensor_config_setup\n");
	err = tsensor_config_setup(data);
	if (err) {
		dev_err(&pdev->dev, "[%s,line=%d]: tsensor counters dead!\n",
			__func__, __LINE__);
		goto err_setup;
	}
	dev_dbg(&pdev->dev, "before tsensor_get_const_AB\n");
	/* calculate constants needed for temperature conversion */
	err = tsensor_get_const_AB(data);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed to extract temperature\n"
			"const\n");
		goto err_setup;
	}

	/* test if counter-to-temperature and temperature-to-counter
	 * are matching */
	err = test_temperature_algo(data);
	if (err) {
		dev_err(&pdev->dev, "Error: read temperature\n"
			"algorithm broken\n");
		goto err_setup;
	}

	print_temperature_2_counter_table(data);

	dev_dbg(&pdev->dev, "before tsensor_threshold_setup\n");
	/* change tsensor threshold for active instance */
	tsensor_threshold_setup(data, data->tsensor_index, false);
	dev_dbg(&pdev->dev, "end tsensor_threshold_setup\n");

	return 0;
err_setup:
	free_irq(data->irq, data);
err_irq:
	tsensor_clk_enable(data, false);
fail:
	dev_err(&pdev->dev, "%s error=%d returned\n", __func__, err);
	return err;
}

static int __devinit tegra_tsensor_probe(struct platform_device *pdev)
{
	struct tegra_tsensor_data *data;
	struct resource *r;
	int err;
	unsigned int reg;
	u8 i;

	data = kzalloc(sizeof(struct tegra_tsensor_data), GFP_KERNEL);
	if (!data) {
		dev_err(&pdev->dev, "[%s,line=%d]: Failed to allocate "
			"memory\n", __func__, __LINE__);
		err = -ENOMEM;
		goto exit;
	}
	platform_set_drvdata(pdev, data);

	/* Register sysfs hooks */
	for (i = 0; i < ARRAY_SIZE(tsensor_nodes); i++) {
		err = device_create_file(&pdev->dev,
			&tsensor_nodes[i].dev_attr);
		if (err) {
			dev_err(&pdev->dev, "device_create_file failed.\n");
			goto err0;
		}
	}

	data->hwmon_dev = hwmon_device_register(&pdev->dev);
	if (IS_ERR(data->hwmon_dev)) {
		err = PTR_ERR(data->hwmon_dev);
		goto err1;
	}

	dev_set_drvdata(data->hwmon_dev, data);

	spin_lock_init(&data->tsensor_lock);

	/* map tsensor register space */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL) {
		dev_err(&pdev->dev, "[%s,line=%d]: Failed to get io "
			"resource\n", __func__, __LINE__);
		err = -ENODEV;
		goto err2;
	}

	if (!request_mem_region(r->start, (r->end - r->start) + 1,
				dev_name(&pdev->dev))) {
		dev_err(&pdev->dev, "[%s,line=%d]: Error mem busy\n",
			__func__, __LINE__);
		err = -EBUSY;
		goto err2;
	}

	data->phys = r->start;
	data->phys_end = r->end;
	data->base = ioremap(r->start, r->end - r->start + 1);
	if (!data->base) {
		dev_err(&pdev->dev, "[%s, line=%d]: can't ioremap "
			"tsensor iomem\n", __FILE__, __LINE__);
		err = -ENOMEM;
		goto err3;
	}

	/* map pmc rst_status register  */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (r == NULL) {
		dev_err(&pdev->dev, "[%s,line=%d]: Failed to get io "
			"resource\n", __func__, __LINE__);
		err = -ENODEV;
		goto err4;
	}

	if (!request_mem_region(r->start, (r->end - r->start) + 1,
				dev_name(&pdev->dev))) {
		dev_err(&pdev->dev, "[%s, line=%d]: Error mem busy\n",
			__func__, __LINE__);
		err = -EBUSY;
		goto err4;
	}

	data->pmc_phys = r->start;
	data->pmc_phys_end = r->end;
	data->pmc_rst_base = ioremap(r->start, r->end - r->start + 1);
	if (!data->pmc_rst_base) {
		dev_err(&pdev->dev, "[%s, line=%d]: can't ioremap "
			"pmc iomem\n", __FILE__, __LINE__);
		err = -ENOMEM;
		goto err5;
	}

	/* fuse revisions less than TSENSOR_FUSE_REVISION_DECIMAL_REV1
	 bypass tsensor driver init */
	/* tsensor active instance decided based on fuse revision */
	err = tegra_fuse_get_revision(&reg);
	if (err)
		goto err6;
	/* check for higher revision done first */
	/* instance 0 is used for fuse revision
	 TSENSOR_FUSE_REVISION_DECIMAL_REV2 onwards */
	if (reg >= TSENSOR_FUSE_REVISION_DECIMAL_REV2)
		data->tsensor_index = 0;
	/* instance 1 is used for fuse revision
	 TSENSOR_FUSE_REVISION_DECIMAL_REV1 till
	 TSENSOR_FUSE_REVISION_DECIMAL_REV2 */
	else if (reg >= TSENSOR_FUSE_REVISION_DECIMAL_REV1)
		data->tsensor_index = 1;
	pr_info("tsensor active instance=%d\n", data->tsensor_index);

	/* tegra tsensor - setup and init */
	err = tegra_tsensor_setup(pdev);
	if (err)
		goto err6;

	dump_tsensor_regs(data);
	dev_dbg(&pdev->dev, "end tegra_tsensor_probe\n");
	return 0;
err6:
	iounmap(data->pmc_rst_base);
err5:
	release_mem_region(data->pmc_phys, (data->pmc_phys_end -
		data->pmc_phys) + 1);
err4:
	iounmap(data->base);
err3:
	release_mem_region(data->phys, (data->phys_end -
		data->phys) + 1);
err2:
	hwmon_device_unregister(data->hwmon_dev);
err1:
	for (i = 0; i < ARRAY_SIZE(tsensor_nodes); i++)
		device_remove_file(&pdev->dev, &tsensor_nodes[i].dev_attr);
err0:
	kfree(data);
exit:
	dev_err(&pdev->dev, "%s error=%d returned\n", __func__, err);
	return err;
}

static int __devexit tegra_tsensor_remove(struct platform_device *pdev)
{
	struct tegra_tsensor_data *data = platform_get_drvdata(pdev);
	u8 i;

	hwmon_device_unregister(data->hwmon_dev);
	for (i = 0; i < ARRAY_SIZE(tsensor_nodes); i++)
		device_remove_file(&pdev->dev, &tsensor_nodes[i].dev_attr);

	free_irq(data->irq, data);

	iounmap(data->pmc_rst_base);
	release_mem_region(data->pmc_phys, (data->pmc_phys_end -
		data->pmc_phys) + 1);
	iounmap(data->base);
	release_mem_region(data->phys, (data->phys_end -
		data->phys) + 1);

	kfree(data);

	return 0;
}

static void save_tsensor_regs(struct tegra_tsensor_data *data)
{
	int i;
	for (i = 0; i < TSENSOR_COUNT; i++) {
		data->config0[i] = tsensor_readl(data,
			((i << 16) | SENSOR_CFG0));
		data->config1[i] = tsensor_readl(data,
			((i << 16) | SENSOR_CFG1));
		data->config2[i] = tsensor_readl(data,
			((i << 16) | SENSOR_CFG2));
	}
}

static void restore_tsensor_regs(struct tegra_tsensor_data *data)
{
	int i;
	for (i = 0; i < TSENSOR_COUNT; i++) {
		tsensor_writel(data, data->config0[i],
			((i << 16) | SENSOR_CFG0));
		tsensor_writel(data, data->config1[i],
			((i << 16) | SENSOR_CFG1));
		tsensor_writel(data, data->config2[i],
			((i << 16) | SENSOR_CFG2));
	}
}

#ifdef CONFIG_PM
static int tsensor_suspend(struct platform_device *pdev,
	pm_message_t state)
{
	struct tegra_tsensor_data *data = platform_get_drvdata(pdev);
	unsigned int config0;
	int i;
	/* set STOP bit, else OVERFLOW interrupt seen in LP1 */
	for (i = 0; i < TSENSOR_COUNT; i++) {
		config0 = tsensor_readl(data, ((i << 16) | SENSOR_CFG0));
		config0 |= (1 << SENSOR_CFG0_STOP_SHIFT);
		tsensor_writel(data, config0, ((i << 16) | SENSOR_CFG0));
	}
	/* save current settings before suspend, when STOP bit is set */
	save_tsensor_regs(data);
	tsensor_clk_enable(data, false);

	return 0;
}

static int tsensor_resume(struct platform_device *pdev)
{
	struct tegra_tsensor_data *data = platform_get_drvdata(pdev);
	unsigned int config0;
	int i;
	tsensor_clk_enable(data, true);
	/* restore current settings before suspend, no need
	 * to clear STOP bit */
	restore_tsensor_regs(data);
	/* clear STOP bit, after restoring regs */
	for (i = 0; i < TSENSOR_COUNT; i++) {
		config0 = tsensor_readl(data, ((i << 16) | SENSOR_CFG0));
		config0 &= ~(1 << SENSOR_CFG0_STOP_SHIFT);
		tsensor_writel(data, config0, ((i << 16) | SENSOR_CFG0));
	}

	return 0;
}
#endif

static struct platform_driver tegra_tsensor_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "tegra-tsensor",
	},
	.probe		= tegra_tsensor_probe,
	.remove		= __devexit_p(tegra_tsensor_remove),
#ifdef CONFIG_PM
	.suspend	= tsensor_suspend,
	.resume		= tsensor_resume,
#endif
};

static int __init tegra_tsensor_init(void)
{
	init_flag = 0;
	if (platform_driver_register(&tegra_tsensor_driver))
		goto exit;
	init_flag = 1;
	return 0;

exit:
	return -ENODEV;
}

static void __exit tegra_tsensor_exit(void)
{
	if (init_flag) {
		platform_driver_unregister(&tegra_tsensor_driver);
		init_flag = 0;
	}
}

MODULE_AUTHOR("nvidia");
MODULE_DESCRIPTION("Nvidia Tegra Temperature Sensor driver");
MODULE_LICENSE("GPL");

module_init(tegra_tsensor_init);
module_exit(tegra_tsensor_exit);

