// SPDX-License-Identifier: GPL-2.0-only
/*
 * IDT P9220 wireless power receiver monitor.
 *
 * This keeps the standard Linux power_supply part of Xiaomi's downstream
 * IDTP9220 support and leaves vendor fast-charge negotiation for later.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define IDTP9220_DRIVER_NAME		"idtp9220"

#define IDTP9220_REG_STATUS_L		0x0034
#define IDTP9220_REG_INTR_L		0x0036
#define IDTP9220_REG_ADC_VOUT_L		0x003c
#define IDTP9220_REG_ADC_VRECT		0x0040
#define IDTP9220_REG_RX_LOUT_L		0x0044
#define IDTP9220_REG_ILIM_SET		0x004a
#define IDTP9220_REG_SSCMND		0x004e
#define IDTP9220_REG_RX_RESET		0x004f
#define IDTP9220_REG_SSINTCLR		0x0056
#define IDTP9220_REG_REGULATOR_L	0x000c
#define IDTP9220_REG_REGULATOR_H	0x000d

#define IDTP9220_STATUS_VOUT_ON		BIT(7)
#define IDTP9220_CLRINT			BIT(5)

#define IDTP9220_VOUT_ADC_SCALE_NUM	(10 * 21 * 1000)
#define IDTP9220_VOUT_ADC_SCALE_DEN	40950
#define IDTP9220_VOUT_ADJUST_MV		35

struct idtp9220 {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *power_good_gpio;
	struct delayed_work irq_work;
	struct delayed_work power_good_work;
	struct mutex lock;
	bool present;
};

static const struct regmap_config idtp9220_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

static int idtp9220_read_word(struct idtp9220 *chip, unsigned int reg, u16 *value)
{
	unsigned int low;
	unsigned int high;
	int ret;

	ret = regmap_read(chip->regmap, reg, &low);
	if (ret)
		return ret;

	ret = regmap_read(chip->regmap, reg + 1, &high);
	if (ret)
		return ret;

	*value = low | (high << 8);
	return 0;
}

static int idtp9220_get_vout_uv(struct idtp9220 *chip, int *microvolts)
{
	u16 raw;
	int ret;

	ret = idtp9220_read_word(chip, IDTP9220_REG_ADC_VOUT_L, &raw);
	if (ret)
		return ret;

	raw &= 0x0fff;
	*microvolts = ((raw * IDTP9220_VOUT_ADC_SCALE_NUM) /
			 IDTP9220_VOUT_ADC_SCALE_DEN + IDTP9220_VOUT_ADJUST_MV) * 1000;

	return 0;
}

static int idtp9220_get_vrect_uv(struct idtp9220 *chip, int *microvolts)
{
	u16 raw;
	int ret;

	ret = idtp9220_read_word(chip, IDTP9220_REG_ADC_VRECT, &raw);
	if (ret)
		return ret;

	raw &= 0x0fff;
	*microvolts = ((raw * IDTP9220_VOUT_ADC_SCALE_NUM) /
			 IDTP9220_VOUT_ADC_SCALE_DEN) * 1000;

	return 0;
}

static int idtp9220_get_iout_ua(struct idtp9220 *chip, int *microamps)
{
	u16 milliamps;
	int ret;

	ret = idtp9220_read_word(chip, IDTP9220_REG_RX_LOUT_L, &milliamps);
	if (ret)
		return ret;

	*microamps = milliamps * 1000;
	return 0;
}

static int idtp9220_get_input_voltage_limit_uv(struct idtp9220 *chip,
						       int *microvolts)
{
	u16 millivolts;
	int ret;

	ret = idtp9220_read_word(chip, IDTP9220_REG_REGULATOR_L, &millivolts);
	if (ret)
		return ret;

	*microvolts = millivolts * 1000;
	return 0;
}

static int idtp9220_set_input_voltage_limit_uv(struct idtp9220 *chip,
						       int microvolts)
{
	unsigned int millivolts = microvolts / 1000;
	int ret;

	if (millivolts < 4900 || millivolts > 10000)
		return -EINVAL;

	mutex_lock(&chip->lock);
	ret = regmap_write(chip->regmap, IDTP9220_REG_REGULATOR_L,
			   millivolts & 0xff);
	if (!ret)
		ret = regmap_write(chip->regmap, IDTP9220_REG_REGULATOR_H,
				   millivolts >> 8);
	mutex_unlock(&chip->lock);

	return ret;
}

static int idtp9220_read_present(struct idtp9220 *chip)
{
	unsigned int status;
	int power_good;
	int ret;

	if (chip->power_good_gpio) {
		power_good = gpiod_get_value_cansleep(chip->power_good_gpio);
		if (power_good < 0)
			return power_good;

		return power_good;
	}

	ret = regmap_read(chip->regmap, IDTP9220_REG_STATUS_L, &status);
	if (ret)
		return ret;

	return !!(status & IDTP9220_STATUS_VOUT_ON);
}

static int idtp9220_get_property(struct power_supply *psy,
					 enum power_supply_property property,
					 union power_supply_propval *value)
{
	struct idtp9220 *chip = power_supply_get_drvdata(psy);
	unsigned int reg_value;
	bool requires_receiver;
	int ret;

	requires_receiver = property == POWER_SUPPLY_PROP_VOLTAGE_NOW ||
			    property == POWER_SUPPLY_PROP_CURRENT_NOW ||
			    property == POWER_SUPPLY_PROP_VOLTAGE_OCV ||
			    property == POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT ||
			    property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;

	if (requires_receiver) {
		ret = idtp9220_read_present(chip);
		if (ret < 0)
			return ret;
		if (!ret) {
			value->intval = 0;
			return 0;
		}
	}

	switch (property) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		ret = idtp9220_read_present(chip);
		if (ret < 0)
			return ret;
		value->intval = ret;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = idtp9220_read_present(chip);
		if (ret < 0)
			return ret;
		value->intval = ret ? POWER_SUPPLY_STATUS_CHARGING :
					    POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return idtp9220_get_vout_uv(chip, &value->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return idtp9220_get_iout_ua(chip, &value->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		return idtp9220_get_vrect_uv(chip, &value->intval);
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		return idtp9220_get_input_voltage_limit_uv(chip, &value->intval);
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = regmap_read(chip->regmap, IDTP9220_REG_ILIM_SET, &reg_value);
		if (ret)
			return ret;
		value->intval = (reg_value + 1) * 100000;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		value->strval = "IDTP9220";
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		value->strval = "Renesas IDT";
		return 0;
	default:
		return -EINVAL;
	}
}

static int idtp9220_set_property(struct power_supply *psy,
					 enum power_supply_property property,
					 const union power_supply_propval *value)
{
	struct idtp9220 *chip = power_supply_get_drvdata(psy);
	int ret;

	switch (property) {
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		ret = idtp9220_read_present(chip);
		if (ret <= 0)
			return ret < 0 ? ret : -ENODEV;
		return idtp9220_set_input_voltage_limit_uv(chip, value->intval);
	default:
		return -EINVAL;
	}
}

static int idtp9220_property_is_writeable(struct power_supply *psy,
						  enum power_supply_property property)
{
	return property == POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT;
}

static enum power_supply_property idtp9220_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static const struct power_supply_desc idtp9220_power_supply_desc = {
	.name = "idtp9220-wireless",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = idtp9220_properties,
	.num_properties = ARRAY_SIZE(idtp9220_properties),
	.get_property = idtp9220_get_property,
	.set_property = idtp9220_set_property,
	.property_is_writeable = idtp9220_property_is_writeable,
};

static void idtp9220_update_present(struct idtp9220 *chip)
{
	int present = idtp9220_read_present(chip);

	if (present < 0)
		return;

	if (chip->present != present) {
		chip->present = present;
		power_supply_changed(chip->psy);
	}
}

static void idtp9220_clear_interrupts(struct idtp9220 *chip)
{
	u16 interrupts;

	if (idtp9220_read_word(chip, IDTP9220_REG_INTR_L, &interrupts))
		return;

	if (!interrupts)
		return;

	regmap_write(chip->regmap, IDTP9220_REG_SSINTCLR, interrupts & 0xff);
	regmap_write(chip->regmap, IDTP9220_REG_SSINTCLR + 1, interrupts >> 8);
	regmap_write(chip->regmap, IDTP9220_REG_SSCMND, IDTP9220_CLRINT);
}

static void idtp9220_irq_work(struct work_struct *work)
{
	struct idtp9220 *chip = container_of(work, struct idtp9220, irq_work.work);

	idtp9220_clear_interrupts(chip);
	idtp9220_update_present(chip);
}

static void idtp9220_power_good_work(struct work_struct *work)
{
	struct idtp9220 *chip = container_of(work, struct idtp9220,
					       power_good_work.work);

	idtp9220_update_present(chip);
}

static irqreturn_t idtp9220_irq_handler(int irq, void *data)
{
	struct idtp9220 *chip = data;

	schedule_delayed_work(&chip->irq_work, msecs_to_jiffies(30));
	return IRQ_HANDLED;
}

static irqreturn_t idtp9220_power_good_irq_handler(int irq, void *data)
{
	struct idtp9220 *chip = data;

	schedule_delayed_work(&chip->power_good_work, 0);
	return IRQ_HANDLED;
}

static int idtp9220_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = { };
	struct idtp9220 *chip;
	int irq;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&client->dev, -EOPNOTSUPP,
				     "I2C transfers are not supported\n");

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &idtp9220_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(chip->regmap),
				     "failed to initialize regmap\n");

	chip->enable_gpio = devm_gpiod_get_optional(&client->dev, "enable",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(chip->enable_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(chip->enable_gpio),
				     "failed to request enable GPIO\n");

	chip->power_good_gpio = devm_gpiod_get_optional(&client->dev,
							 "power-good", GPIOD_IN);
	if (IS_ERR(chip->power_good_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(chip->power_good_gpio),
				     "failed to request power-good GPIO\n");

	INIT_DELAYED_WORK(&chip->irq_work, idtp9220_irq_work);
	INIT_DELAYED_WORK(&chip->power_good_work, idtp9220_power_good_work);
	device_init_wakeup(&client->dev, true);

	psy_cfg.drv_data = chip;
	chip->psy = devm_power_supply_register(&client->dev,
						  &idtp9220_power_supply_desc,
						  &psy_cfg);
	if (IS_ERR(chip->psy))
		return dev_err_probe(&client->dev, PTR_ERR(chip->psy),
				     "failed to register power supply\n");

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					       idtp9220_irq_handler, IRQF_ONESHOT,
					       client->name, chip);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to request IRQ\n");

		enable_irq_wake(client->irq);
	}

	if (chip->power_good_gpio) {
		irq = gpiod_to_irq(chip->power_good_gpio);
		if (irq < 0)
			return dev_err_probe(&client->dev, irq,
					     "failed to map power-good IRQ\n");

		ret = devm_request_threaded_irq(&client->dev, irq, NULL,
					       idtp9220_power_good_irq_handler,
					       IRQF_ONESHOT | IRQF_TRIGGER_RISING |
					       IRQF_TRIGGER_FALLING,
					       "idtp9220-power-good", chip);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to request power-good IRQ\n");

		enable_irq_wake(irq);
	}

	idtp9220_update_present(chip);

	return 0;
}

static void idtp9220_remove(struct i2c_client *client)
{
	struct idtp9220 *chip = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chip->irq_work);
	cancel_delayed_work_sync(&chip->power_good_work);
}

static void idtp9220_shutdown(struct i2c_client *client)
{
	struct idtp9220 *chip = i2c_get_clientdata(client);

	if (chip && chip->present)
		regmap_write(chip->regmap, IDTP9220_REG_RX_RESET, 0x01);
}

static const struct of_device_id idtp9220_of_match[] = {
	{ .compatible = "idt,p9220" },
	{ .compatible = "renesas,idtp9220" },
	{ }
};
MODULE_DEVICE_TABLE(of, idtp9220_of_match);

static const struct i2c_device_id idtp9220_i2c_id[] = {
	{ "idtp9220" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, idtp9220_i2c_id);

static struct i2c_driver idtp9220_driver = {
	.driver = {
		.name = IDTP9220_DRIVER_NAME,
		.of_match_table = idtp9220_of_match,
	},
	.probe = idtp9220_probe,
	.remove = idtp9220_remove,
	.shutdown = idtp9220_shutdown,
	.id_table = idtp9220_i2c_id,
};
module_i2c_driver(idtp9220_driver);

MODULE_DESCRIPTION("IDT P9220 wireless power receiver monitor");
MODULE_LICENSE("GPL");
