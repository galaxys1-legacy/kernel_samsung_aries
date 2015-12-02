/*
<<<<<<< HEAD
 * linux/drivers/power/max8998_charger.c
 *
 * Charger driver of MAX8998 PMIC
 *
 *  Copyright (C) 2010 Samsung Electronics
 *  Ikkeun Kim <iks.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/power/sec_battery.h>
#include <linux/mfd/max8998.h>
#include <linux/mfd/max8998-private.h>
#include <linux/regulator/driver.h>
#include <linux/slab.h>


struct max8998_chg_data {
	struct device		*dev;
	struct max8998_dev	*iodev;
	struct max8998_charger_data *pdata;
	struct charger_device *chgdev;
};

static struct max8998_chg_data *max8998_chg;  // for local use


static int max8998_check_vdcin(void)
{
	struct max8998_chg_data *chg = max8998_chg;
	u8 data = 0;
	int ret;

	ret = max8998_read_reg(chg->iodev->i2c, MAX8998_REG_STATUS2, &data);

	if (ret < 0) {
		pr_err("max8998_read_reg error\n");
		return ret;
	}

	return data & MAX8998_MASK_VDCIN;
}

static int max8998_charging_control(int en, int cable_status)
{
	struct max8998_chg_data *chg = max8998_chg;
	struct i2c_client *i2c = chg->iodev->i2c;
	int ret = 0;

	pr_info("%s : conn=%d, cable_status=%d\n", __func__, en, cable_status);

	if (!en) {
		/* disable charging */
		ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2,
			(1 << MAX8998_SHIFT_CHGEN), MAX8998_MASK_CHGEN);
		if (ret < 0)
			goto err;

		pr_info("%s : charging disabled\n", __func__);
	} else {
		/* enable charging */
		if (cable_status == CABLE_TYPE_AC) {
			/* ac */
			ret = max8998_update_reg(i2c, MAX8998_REG_CHGR1,
				(2 << MAX8998_SHIFT_TOPOFF), MAX8998_MASK_TOPOFF);
			if (ret < 0)
				goto err;

			ret = max8998_update_reg(i2c, MAX8998_REG_CHGR1,
				(5 << MAX8998_SHIFT_ICHG), MAX8998_MASK_ICHG);
			if (ret < 0)
				goto err;

			ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2,
				(2 << MAX8998_SHIFT_ESAFEOUT), MAX8998_MASK_ESAFEOUT);
			if (ret < 0)
				goto err;

			pr_info("%s : TA charging enabled\n", __func__);

		} else {
			/* usb */
			ret = max8998_update_reg(i2c, MAX8998_REG_CHGR1,
				(6 << MAX8998_SHIFT_TOPOFF), MAX8998_MASK_TOPOFF);
			if (ret < 0)
				goto err;

			ret = max8998_update_reg(i2c, MAX8998_REG_CHGR1,
				(2 << MAX8998_SHIFT_ICHG), MAX8998_MASK_ICHG);
			if (ret < 0)
				goto err;

			ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2,
				(3 << MAX8998_SHIFT_ESAFEOUT), MAX8998_MASK_ESAFEOUT);
			if (ret < 0)
				goto err;

			pr_debug("%s : USB charging enabled", __func__);
		}

		ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2,
			(0 << MAX8998_SHIFT_CHGEN), MAX8998_MASK_CHGEN);
		if (ret < 0)
			goto err;
	}

	return 0;

err:
	pr_err("max8998_read_reg error\n");
	return ret;
}

static __devinit int max8998_charger_probe(struct platform_device *pdev)
{
	struct max8998_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct max8998_platform_data *pdata = dev_get_platdata(iodev->dev);
	struct max8998_chg_data *chg;
	struct i2c_client *i2c = iodev->i2c;
	int ret = 0;

	pr_info("%s : MAX8998 Charger Driver Loading\n", __func__);

	chg = kzalloc(sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->iodev = iodev;
	chg->pdata = pdata->charger;
	chg->chgdev = chg->pdata->chgdev;

	max8998_chg = chg;  // set local

	if (!chg->pdata) {
		pr_err("%s : No platform data supplied\n", __func__);
		ret = -EINVAL;
		goto err_pdata;
	}

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR1, /* disable */
		(0x3 << MAX8998_SHIFT_RSTR), MAX8998_MASK_RSTR);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2, /* 6 Hr */
		(0x2 << MAX8998_SHIFT_FT), MAX8998_MASK_FT);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2, /* 4.2V */
		(0x0 << MAX8998_SHIFT_BATTSL), MAX8998_MASK_BATTSL);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2, /* 105c */
		(0x0 << MAX8998_SHIFT_TMP), MAX8998_MASK_TMP);
	if (ret < 0)
		goto err_kfree;

	chg->chgdev->charging_control = max8998_charging_control;
	chg->chgdev->get_connection_status = max8998_check_vdcin;
	chg->chgdev->get_charging_status = NULL;

	if(chg->pdata && chg->pdata->charger_dev_register)
		chg->pdata->charger_dev_register(chg->chgdev);

	return 0;

err_kfree:
err_pdata:
	kfree(chg);
	return ret;
}

static int __devexit max8998_charger_remove(struct platform_device *pdev)
{
	struct max8998_chg_data *chg = platform_get_drvdata(pdev);

	if(chg->pdata && chg->pdata->charger_dev_unregister)
		chg->pdata->charger_dev_unregister(chg->chgdev);

	kfree(chg);
=======
 * max8998_charger.c - Power supply consumer driver for the Maxim 8998/LP3974
 *
 *  Copyright (C) 2009-2010 Samsung Electronics
 *  MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/mfd/max8998.h>
#include <linux/mfd/max8998-private.h>

struct max8998_battery_data {
	struct device *dev;
	struct max8998_dev *iodev;
	struct power_supply battery;
};

static enum power_supply_property max8998_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT, /* the presence of battery */
	POWER_SUPPLY_PROP_ONLINE, /* charger is active or not */
};

/* Note that the charger control is done by a current regulator "CHARGER" */
static int max8998_battery_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct max8998_battery_data *max8998 = container_of(psy,
			struct max8998_battery_data, battery);
	struct i2c_client *i2c = max8998->iodev->i2c;
	int ret;
	u8 reg;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		ret = max8998_read_reg(i2c, MAX8998_REG_STATUS2, &reg);
		if (ret)
			return ret;
		if (reg & (1 << 4))
			val->intval = 0;
		else
			val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = max8998_read_reg(i2c, MAX8998_REG_STATUS2, &reg);
		if (ret)
			return ret;
		if (reg & (1 << 3))
			val->intval = 0;
		else
			val->intval = 1;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static __devinit int max8998_battery_probe(struct platform_device *pdev)
{
	struct max8998_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct max8998_platform_data *pdata = dev_get_platdata(iodev->dev);
	struct max8998_battery_data *max8998;
	struct i2c_client *i2c;
	int ret = 0;

	if (!pdata) {
		dev_err(pdev->dev.parent, "No platform init data supplied\n");
		return -ENODEV;
	}

	max8998 = kzalloc(sizeof(struct max8998_battery_data), GFP_KERNEL);
	if (!max8998)
		return -ENOMEM;

	max8998->dev = &pdev->dev;
	max8998->iodev = iodev;
	platform_set_drvdata(pdev, max8998);
	i2c = max8998->iodev->i2c;

	/* Setup "End of Charge" */
	/* If EOC value equals 0,
	 * remain value set from bootloader or default value */
	if (pdata->eoc >= 10 && pdata->eoc <= 45) {
		max8998_update_reg(i2c, MAX8998_REG_CHGR1,
				(pdata->eoc / 5 - 2) << 5, 0x7 << 5);
	} else if (pdata->eoc == 0) {
		dev_dbg(max8998->dev,
			"EOC value not set: leave it unchanged.\n");
	} else {
		dev_err(max8998->dev, "Invalid EOC value\n");
		ret = -EINVAL;
		goto err;
	}

	/* Setup Charge Restart Level */
	switch (pdata->restart) {
	case 100:
		max8998_update_reg(i2c, MAX8998_REG_CHGR1, 0x1 << 3, 0x3 << 3);
		break;
	case 150:
		max8998_update_reg(i2c, MAX8998_REG_CHGR1, 0x0 << 3, 0x3 << 3);
		break;
	case 200:
		max8998_update_reg(i2c, MAX8998_REG_CHGR1, 0x2 << 3, 0x3 << 3);
		break;
	case -1:
		max8998_update_reg(i2c, MAX8998_REG_CHGR1, 0x3 << 3, 0x3 << 3);
		break;
	case 0:
		dev_dbg(max8998->dev,
			"Restart Level not set: leave it unchanged.\n");
		break;
	default:
		dev_err(max8998->dev, "Invalid Restart Level\n");
		ret = -EINVAL;
		goto err;
	}

	/* Setup Charge Full Timeout */
	switch (pdata->timeout) {
	case 5:
		max8998_update_reg(i2c, MAX8998_REG_CHGR2, 0x0 << 4, 0x3 << 4);
		break;
	case 6:
		max8998_update_reg(i2c, MAX8998_REG_CHGR2, 0x1 << 4, 0x3 << 4);
		break;
	case 7:
		max8998_update_reg(i2c, MAX8998_REG_CHGR2, 0x2 << 4, 0x3 << 4);
		break;
	case -1:
		max8998_update_reg(i2c, MAX8998_REG_CHGR2, 0x3 << 4, 0x3 << 4);
		break;
	case 0:
		dev_dbg(max8998->dev,
			"Full Timeout not set: leave it unchanged.\n");
	default:
		dev_err(max8998->dev, "Invalid Full Timeout value\n");
		ret = -EINVAL;
		goto err;
	}

	max8998->battery.name = "max8998_pmic";
	max8998->battery.type = POWER_SUPPLY_TYPE_BATTERY;
	max8998->battery.get_property = max8998_battery_get_property;
	max8998->battery.properties = max8998_battery_props;
	max8998->battery.num_properties = ARRAY_SIZE(max8998_battery_props);

	ret = power_supply_register(max8998->dev, &max8998->battery);
	if (ret) {
		dev_err(max8998->dev, "failed: power supply register\n");
		goto err;
	}

	return 0;
err:
	kfree(max8998);
	return ret;
}

static int __devexit max8998_battery_remove(struct platform_device *pdev)
{
	struct max8998_battery_data *max8998 = platform_get_drvdata(pdev);

	power_supply_unregister(&max8998->battery);
	kfree(max8998);
>>>>>>> v3.1

	return 0;
}

<<<<<<< HEAD
static struct platform_driver max8998_charger_driver = {
	.driver = {
		.name = "max8998-charger",
		.owner = THIS_MODULE,
	},
	.probe = max8998_charger_probe,
	.remove = __devexit_p(max8998_charger_remove),
};

static int __init max8998_charger_init(void)
{
	return platform_driver_register(&max8998_charger_driver);
}

static void __exit max8998_charger_exit(void)
{
	platform_driver_register(&max8998_charger_driver);
}

module_init(max8998_charger_init);
module_exit(max8998_charger_exit);

MODULE_AUTHOR("Ikkeun Kim <iks.kim@samsung.com>");
MODULE_DESCRIPTION("max8998 charger driver");
MODULE_LICENSE("GPL");
=======
static const struct platform_device_id max8998_battery_id[] = {
	{ "max8998-battery", TYPE_MAX8998 },
};

static struct platform_driver max8998_battery_driver = {
	.driver = {
		.name = "max8998-battery",
		.owner = THIS_MODULE,
	},
	.probe = max8998_battery_probe,
	.remove = __devexit_p(max8998_battery_remove),
	.id_table = max8998_battery_id,
};

static int __init max8998_battery_init(void)
{
	return platform_driver_register(&max8998_battery_driver);
}
module_init(max8998_battery_init);

static void __exit max8998_battery_cleanup(void)
{
	platform_driver_unregister(&max8998_battery_driver);
}
module_exit(max8998_battery_cleanup);

MODULE_DESCRIPTION("MAXIM 8998 battery control driver");
MODULE_AUTHOR("MyungJoo Ham <myungjoo.ham@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:max8998-battery");
>>>>>>> v3.1
