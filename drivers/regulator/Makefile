#
# Makefile for regulator drivers.
#


obj-$(CONFIG_REGULATOR) += core.o dummy.o
obj-$(CONFIG_REGULATOR_FIXED_VOLTAGE) += fixed.o
obj-$(CONFIG_REGULATOR_VIRTUAL_ADJ_VOLTAGE) += virtual_adj.o
obj-$(CONFIG_REGULATOR_VIRTUAL_CONSUMER) += virtual.o
obj-$(CONFIG_REGULATOR_USERSPACE_CONSUMER) += userspace-consumer.o

obj-$(CONFIG_REGULATOR_AD5398) += ad5398.o
obj-$(CONFIG_REGULATOR_BQ24022) += bq24022.o
#obj-$(CONFIG_REGULATOR_BQ24610) += bq24610.o
obj-$(CONFIG_REGULATOR_LP3971) += lp3971.o
obj-$(CONFIG_REGULATOR_LP3972) += lp3972.o
obj-$(CONFIG_REGULATOR_MAX1586) += max1586.o
obj-$(CONFIG_REGULATOR_TWL4030) += twl-regulator.o
obj-$(CONFIG_REGULATOR_MAX8649)	+= max8649.o
obj-$(CONFIG_REGULATOR_MAX8660) += max8660.o
obj-$(CONFIG_REGULATOR_MAX8925) += max8925-regulator.o
obj-$(CONFIG_REGULATOR_MAX8952) += max8952.o
obj-$(CONFIG_REGULATOR_MAX8997) += max8997.o
obj-$(CONFIG_REGULATOR_MAX8998) += max8998.o
obj-$(CONFIG_REGULATOR_MAX8907C) += max8907c-regulator.o
obj-$(CONFIG_REGULATOR_MAX77663) += max77663-regulator.o
obj-$(CONFIG_REGULATOR_WM831X) += wm831x-dcdc.o
obj-$(CONFIG_REGULATOR_WM831X) += wm831x-isink.o
obj-$(CONFIG_REGULATOR_WM831X) += wm831x-ldo.o
obj-$(CONFIG_REGULATOR_WM8350) += wm8350-regulator.o
obj-$(CONFIG_REGULATOR_WM8400) += wm8400-regulator.o
obj-$(CONFIG_REGULATOR_WM8994) += wm8994-regulator.o
obj-$(CONFIG_REGULATOR_TPS6586X) += tps6586x-regulator.o
obj-$(CONFIG_REGULATOR_TPS6591X) += tps6591x-regulator.o
obj-$(CONFIG_REGULATOR_DA903X)	+= da903x.o
obj-$(CONFIG_REGULATOR_PCF50633) += pcf50633-regulator.o
obj-$(CONFIG_REGULATOR_PCAP) += pcap-regulator.o
obj-$(CONFIG_REGULATOR_MC13783) += mc13783-regulator.o
obj-$(CONFIG_REGULATOR_MC13892) += mc13892-regulator.o
obj-$(CONFIG_REGULATOR_MC13XXX_CORE) +=  mc13xxx-regulator-core.o
obj-$(CONFIG_REGULATOR_AB3100) += ab3100.o
obj-$(CONFIG_REGULATOR_TPS6105X) += tps6105x-regulator.o
obj-$(CONFIG_REGULATOR_TPS65023) += tps65023-regulator.o
obj-$(CONFIG_REGULATOR_TPS6507X) += tps6507x-regulator.o
obj-$(CONFIG_REGULATOR_TPS6524X) += tps6524x-regulator.o
obj-$(CONFIG_REGULATOR_TPS6236X) += tps6236x-regulator.o
obj-$(CONFIG_REGULATOR_TPS80031) += tps80031-regulator.o
obj-$(CONFIG_REGULATOR_RICOH583) += ricoh583-regulator.o
obj-$(CONFIG_REGULATOR_88PM8607) += 88pm8607.o
obj-$(CONFIG_REGULATOR_ISL6271A) += isl6271a-regulator.o
obj-$(CONFIG_REGULATOR_AB8500)	+= ab8500.o
obj-$(CONFIG_REGULATOR_GPIO_SWITCH)	+= gpio-switch-regulator.o
obj-$(CONFIG_REGULATOR_AAT2870) += aat2870-regulator.o
ccflags-$(CONFIG_REGULATOR_DEBUG) += -DDEBUG
