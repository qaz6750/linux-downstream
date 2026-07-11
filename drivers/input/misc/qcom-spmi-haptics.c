// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm SPMI haptics driver for PMIC haptics blocks.
 *
 * Based on the upstream-style SPMI FF-memless driver model and the
 * downstream QTI haptics register programming used by PM8150B devices.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define HAP_STATUS1_REG			0x0a
#define HAP_SC_DET_BIT			BIT(3)

#define HAP_EN_CTL1_REG			0x46
#define HAP_EN_BIT			BIT(7)

#define HAP_EN_CTL2_REG			0x48
#define HAP_BRAKE_EN_BIT		BIT(0)

#define HAP_EN_CTL3_REG			0x4a
#define HAP_HBRIDGE_EN_BIT		BIT(7)
#define HAP_PWM_SIGNAL_EN_BIT		BIT(6)
#define HAP_ILIM_EN_BIT			BIT(5)
#define HAP_ILIM_CC_EN_BIT		BIT(4)
#define HAP_AUTO_RES_RBIAS_EN_BIT	BIT(3)
#define HAP_DAC_EN_BIT			BIT(2)
#define HAP_PWM_CTL_EN_BIT		BIT(0)

#define HAP_AUTO_RES_CTRL_REG		0x4b
#define HAP_AUTO_RES_EN_BIT		BIT(7)
#define HAP_SEL_AUTO_RES_PERIOD_BIT	BIT(6)
#define HAP_AUTO_RES_CNT_ERR_DELTA_MASK	GENMASK(5, 4)
#define HAP_AUTO_RES_ERR_RECOVERY_BIT	BIT(3)
#define HAP_AUTO_RES_EN_DLY_MASK	GENMASK(2, 0)

#define HAP_CFG1_REG			0x4c
#define HAP_CFG2_REG			0x4d

#define HAP_SEL_REG			0x4e
#define HAP_WF_SOURCE_MASK		GENMASK(5, 4)
#define HAP_WF_SOURCE_SHIFT		4

#define HAP_AUTO_RES_CFG_REG		0x4f
#define HAP_AUTO_RES_MODE_BIT		BIT(7)
#define HAP_CAL_EOP_EN_BIT		BIT(3)
#define HAP_CAL_PERIOD_MASK		GENMASK(2, 0)

#define HAP_VMAX_CFG_REG		0x51
#define HAP_VMAX_MV_MASK		GENMASK(5, 1)
#define HAP_VMAX_MV_SHIFT		1
#define HAP_VMAX_MV_LSB		116
#define HAP_VMAX_MV_DEFAULT		1800
#define HAP_VMAX_MV_MAX		3596

#define HAP_ILIM_CFG_REG		0x52
#define HAP_ILIM_DEFAULT_SEL		BIT(1)

#define HAP_ZX_CFG_REG			0x5a
#define HAP_ZX_DET_DEB_MASK		GENMASK(2, 0)
#define HAP_ZX_DET_DEB_80_US		3

#define HAP_RATE_CFG1_REG		0x54
#define HAP_PLAY_RATE_US_DEFAULT	5715
#define HAP_PLAY_RATE_US_MAX		20475
#define HAP_PLAY_RATE_US_LSB		5

#define HAP_SC_CLR_REG			0x59
#define HAP_SC_CLR_BIT			BIT(0)

#define HAP_BRAKE_REG			0x5c
#define HAP_BRAKE_PATTERN_MAX		4
#define HAP_BRAKE_PATTERN_MASK		0x3
#define HAP_BRAKE_PATTERN_SHIFT		2

#define HAP_WF_REPEAT_REG		0x5e
#define HAP_WF_REPEAT_MASK		GENMASK(6, 4)
#define HAP_WF_REPEAT_SHIFT		4
#define HAP_WF_S_REPEAT_MASK		GENMASK(1, 0)

#define HAP_WF_S1_REG			0x60
#define HAP_WAVEFORM_BUFFER_MAX	8

#define HAP_PLAY_REG			0x70
#define HAP_PLAY_BIT			BIT(7)

#define HAP_SEC_ACCESS_REG		0xd0
#define HAP_SEC_ACCESS_UNLOCK		0xa5

#define HAP_SC_DET_MAX_COUNT		5
#define HAP_SC_DET_TIME_US		1000000

enum qcom_haptics_actuator_type {
	QCOM_HAPTICS_LRA,
	QCOM_HAPTICS_ERM,
};

enum qcom_haptics_lra_shape {
	QCOM_HAPTICS_LRA_SINE,
	QCOM_HAPTICS_LRA_SQUARE,
};

enum qcom_haptics_auto_res_mode {
	QCOM_HAPTICS_AUTO_RES_ZXD,
	QCOM_HAPTICS_AUTO_RES_QWD,
};

enum qcom_haptics_waveform_source {
	QCOM_HAPTICS_WF_VMAX,
	QCOM_HAPTICS_WF_BUFFER,
};

struct qcom_spmi_haptics {
	struct device *dev;
	struct regmap *regmap;
	struct input_dev *input_dev;
	struct work_struct work;
	struct mutex lock;

	u16 base;
	int play_irq;
	int sc_irq;
	ktime_t last_sc_time;
	int sc_count;

	enum qcom_haptics_actuator_type actuator_type;
	enum qcom_haptics_lra_shape lra_shape;
	enum qcom_haptics_auto_res_mode auto_res_mode;
	u16 vmax_mv;
	u16 play_rate_us;
	u8 waveform[HAP_WAVEFORM_BUFFER_MAX];
	u8 brake[HAP_BRAKE_PATTERN_MAX];
	int waveform_length;

	int magnitude;
	bool active;
	bool permanently_disabled;
};

static bool qcom_spmi_haptics_is_secure(u16 addr)
{
	return (addr & 0xff) > HAP_SEC_ACCESS_REG;
}

static int qcom_spmi_haptics_write(struct qcom_spmi_haptics *haptics,
					   u16 addr, const u8 *buf, int len)
{
	int ret;
	int index;

	if (qcom_spmi_haptics_is_secure(addr)) {
		for (index = 0; index < len; index++) {
			ret = regmap_write(haptics->regmap,
					   haptics->base + HAP_SEC_ACCESS_REG,
					   HAP_SEC_ACCESS_UNLOCK);
			if (ret)
				return ret;

			ret = regmap_write(haptics->regmap, haptics->base + addr + index,
					   buf[index]);
			if (ret)
				return ret;
		}

		return 0;
	}

	if (len == 1)
		return regmap_write(haptics->regmap, haptics->base + addr, *buf);

	return regmap_bulk_write(haptics->regmap, haptics->base + addr, buf, len);
}

static int qcom_spmi_haptics_write_byte(struct qcom_spmi_haptics *haptics,
						u16 addr, u8 value)
{
	return qcom_spmi_haptics_write(haptics, addr, &value, 1);
}

static int qcom_spmi_haptics_write_masked(struct qcom_spmi_haptics *haptics,
						  u16 addr, u8 mask, u8 value)
{
	int ret;

	if (qcom_spmi_haptics_is_secure(addr)) {
		ret = regmap_write(haptics->regmap,
				   haptics->base + HAP_SEC_ACCESS_REG,
				   HAP_SEC_ACCESS_UNLOCK);
		if (ret)
			return ret;
	}

	return regmap_update_bits(haptics->regmap, haptics->base + addr,
				  mask, value);
}

static int qcom_spmi_haptics_read_byte(struct qcom_spmi_haptics *haptics,
					       u16 addr, u8 *value)
{
	unsigned int reg_value;
	int ret;

	ret = regmap_read(haptics->regmap, haptics->base + addr, &reg_value);
	if (ret)
		return ret;

	*value = reg_value;
	return 0;
}

static int qcom_spmi_haptics_set_vmax(struct qcom_spmi_haptics *haptics,
					      u16 vmax_mv)
{
	u8 value;

	vmax_mv = clamp_t(u16, vmax_mv, HAP_VMAX_MV_LSB, HAP_VMAX_MV_MAX);
	value = (vmax_mv / HAP_VMAX_MV_LSB) << HAP_VMAX_MV_SHIFT;

	return qcom_spmi_haptics_write_masked(haptics, HAP_VMAX_CFG_REG,
					       HAP_VMAX_MV_MASK, value);
}

static int qcom_spmi_haptics_set_play_rate(struct qcom_spmi_haptics *haptics,
						   u16 play_rate_us)
{
	u16 register_value = play_rate_us / HAP_PLAY_RATE_US_LSB;
	u8 values[2];

	values[0] = register_value & 0xff;
	values[1] = (register_value >> 8) & 0x0f;

	return qcom_spmi_haptics_write(haptics, HAP_RATE_CFG1_REG, values, 2);
}

static int qcom_spmi_haptics_set_waveform_source(struct qcom_spmi_haptics *haptics,
							 enum qcom_haptics_waveform_source source)
{
	u8 value = source << HAP_WF_SOURCE_SHIFT;

	return qcom_spmi_haptics_write_masked(haptics, HAP_SEL_REG,
					       HAP_WF_SOURCE_MASK, value);
}

static int qcom_spmi_haptics_set_brake(struct qcom_spmi_haptics *haptics)
{
	u8 value = 0;
	int ret;
	int index;

	for (index = 0; index < HAP_BRAKE_PATTERN_MAX; index++)
		value |= (haptics->brake[index] & HAP_BRAKE_PATTERN_MASK) <<
			 (index * HAP_BRAKE_PATTERN_SHIFT);

	ret = qcom_spmi_haptics_write_byte(haptics, HAP_BRAKE_REG, value);
	if (ret)
		return ret;

	return qcom_spmi_haptics_write_masked(haptics, HAP_EN_CTL2_REG,
					       HAP_BRAKE_EN_BIT, HAP_BRAKE_EN_BIT);
}

static int qcom_spmi_haptics_set_auto_res(struct qcom_spmi_haptics *haptics,
						  bool enable)
{
	u8 value = enable ? HAP_AUTO_RES_EN_BIT : 0;

	if (haptics->actuator_type != QCOM_HAPTICS_LRA)
		return 0;

	return qcom_spmi_haptics_write_masked(haptics, HAP_AUTO_RES_CTRL_REG,
					       HAP_AUTO_RES_EN_BIT, value);
}

static int qcom_spmi_haptics_module_enable(struct qcom_spmi_haptics *haptics,
						   bool enable)
{
	u8 value = enable ? HAP_EN_BIT : 0;

	return qcom_spmi_haptics_write_byte(haptics, HAP_EN_CTL1_REG, value);
}

static int qcom_spmi_haptics_play(struct qcom_spmi_haptics *haptics,
					  bool enable)
{
	u8 value = enable ? HAP_PLAY_BIT : 0;

	return qcom_spmi_haptics_write_byte(haptics, HAP_PLAY_REG, value);
}

static int qcom_spmi_haptics_program_buffer(struct qcom_spmi_haptics *haptics)
{
	u8 waveform[HAP_WAVEFORM_BUFFER_MAX] = { };
	u8 repeat = 0;
	int ret;

	memcpy(waveform, haptics->waveform,
	       min(haptics->waveform_length, HAP_WAVEFORM_BUFFER_MAX));

	ret = qcom_spmi_haptics_write(haptics, HAP_WF_S1_REG, waveform,
				       HAP_WAVEFORM_BUFFER_MAX);
	if (ret)
		return ret;

	return qcom_spmi_haptics_write_masked(haptics, HAP_WF_REPEAT_REG,
					       HAP_WF_REPEAT_MASK |
					       HAP_WF_S_REPEAT_MASK, repeat);
}

static int qcom_spmi_haptics_hw_init(struct qcom_spmi_haptics *haptics)
{
	u8 value;
	int ret;

	ret = qcom_spmi_haptics_write_byte(haptics, HAP_CFG1_REG,
					    haptics->actuator_type);
	if (ret)
		return ret;

	ret = qcom_spmi_haptics_write_byte(haptics, HAP_ILIM_CFG_REG,
					    HAP_ILIM_DEFAULT_SEL);
	if (ret)
		return ret;

	value = HAP_HBRIDGE_EN_BIT | HAP_PWM_SIGNAL_EN_BIT | HAP_ILIM_EN_BIT |
		HAP_ILIM_CC_EN_BIT | HAP_AUTO_RES_RBIAS_EN_BIT | HAP_DAC_EN_BIT |
		HAP_PWM_CTL_EN_BIT;
	ret = qcom_spmi_haptics_write_byte(haptics, HAP_EN_CTL3_REG, value);
	if (ret)
		return ret;

	ret = qcom_spmi_haptics_write_masked(haptics, HAP_ZX_CFG_REG,
					       HAP_ZX_DET_DEB_MASK,
					       HAP_ZX_DET_DEB_80_US);
	if (ret)
		return ret;

	ret = qcom_spmi_haptics_set_play_rate(haptics, haptics->play_rate_us);
	if (ret)
		return ret;

	ret = qcom_spmi_haptics_set_vmax(haptics, haptics->vmax_mv);
	if (ret)
		return ret;

	ret = qcom_spmi_haptics_set_brake(haptics);
	if (ret)
		return ret;

	if (haptics->actuator_type == QCOM_HAPTICS_ERM)
		return qcom_spmi_haptics_set_auto_res(haptics, false);

	ret = qcom_spmi_haptics_write_byte(haptics, HAP_CFG2_REG,
					    haptics->lra_shape);
	if (ret)
		return ret;

	value = (haptics->auto_res_mode << 7) | HAP_CAL_EOP_EN_BIT;
	ret = qcom_spmi_haptics_write_masked(haptics, HAP_AUTO_RES_CFG_REG,
					       HAP_AUTO_RES_MODE_BIT |
					       HAP_CAL_EOP_EN_BIT |
					       HAP_CAL_PERIOD_MASK, value);
	if (ret)
		return ret;

	value = HAP_AUTO_RES_EN_BIT | HAP_SEL_AUTO_RES_PERIOD_BIT |
		FIELD_PREP(HAP_AUTO_RES_CNT_ERR_DELTA_MASK, 2) |
		HAP_AUTO_RES_ERR_RECOVERY_BIT |
		FIELD_PREP(HAP_AUTO_RES_EN_DLY_MASK, 4);

	return qcom_spmi_haptics_write_byte(haptics, HAP_AUTO_RES_CTRL_REG, value);
}

static void qcom_spmi_haptics_work(struct work_struct *work)
{
	struct qcom_spmi_haptics *haptics =
		container_of(work, struct qcom_spmi_haptics, work);
	int ret;
	u16 vmax_mv;

	mutex_lock(&haptics->lock);

	if (haptics->active && !haptics->permanently_disabled) {
		vmax_mv = haptics->vmax_mv * haptics->magnitude / 0xff;
		ret = qcom_spmi_haptics_set_vmax(haptics, vmax_mv);
		if (ret)
			goto out;

		ret = qcom_spmi_haptics_program_buffer(haptics);
		if (ret)
			goto out;

		ret = qcom_spmi_haptics_set_waveform_source(haptics,
							 QCOM_HAPTICS_WF_BUFFER);
		if (ret)
			goto out;

		ret = qcom_spmi_haptics_module_enable(haptics, true);
		if (ret)
			goto out;

		ret = qcom_spmi_haptics_play(haptics, true);
	} else {
		ret = qcom_spmi_haptics_play(haptics, false);
		if (ret)
			goto out;

		ret = qcom_spmi_haptics_module_enable(haptics, false);
	}

out:
	if (ret)
		dev_err(haptics->dev, "failed to update haptics state: %d\n", ret);

	mutex_unlock(&haptics->lock);
}

static void qcom_spmi_haptics_close(struct input_dev *input_dev)
{
	struct qcom_spmi_haptics *haptics = input_get_drvdata(input_dev);

	cancel_work_sync(&haptics->work);
	haptics->active = false;
	qcom_spmi_haptics_play(haptics, false);
	qcom_spmi_haptics_module_enable(haptics, false);
}

static int qcom_spmi_haptics_play_effect(struct input_dev *input_dev,
						 void *data,
						 struct ff_effect *effect)
{
	struct qcom_spmi_haptics *haptics = input_get_drvdata(input_dev);
	int magnitude;

	magnitude = effect->u.rumble.strong_magnitude >> 8;
	if (!magnitude)
		magnitude = effect->u.rumble.weak_magnitude >> 8;

	haptics->magnitude = magnitude;
	haptics->active = magnitude > 0;
	schedule_work(&haptics->work);

	return 0;
}

static irqreturn_t qcom_spmi_haptics_play_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

static irqreturn_t qcom_spmi_haptics_sc_irq(int irq, void *data)
{
	struct qcom_spmi_haptics *haptics = data;
	s64 delta_us;
	u8 value;
	int ret;

	ret = qcom_spmi_haptics_read_byte(haptics, HAP_STATUS1_REG, &value);
	if (ret || !(value & HAP_SC_DET_BIT))
		return IRQ_HANDLED;

	delta_us = ktime_us_delta(ktime_get(), haptics->last_sc_time);
	haptics->last_sc_time = ktime_get();
	if (delta_us > HAP_SC_DET_TIME_US)
		haptics->sc_count = 0;
	else
		haptics->sc_count++;

	value = HAP_SC_CLR_BIT;
	qcom_spmi_haptics_write_byte(haptics, HAP_SC_CLR_REG, value);

	if (haptics->sc_count > HAP_SC_DET_MAX_COUNT) {
		haptics->permanently_disabled = true;
		qcom_spmi_haptics_module_enable(haptics, false);
		dev_crit(haptics->dev, "short circuit persists, disabling haptics\n");
	}

	return IRQ_HANDLED;
}

static int qcom_spmi_haptics_parse_waveform(struct qcom_spmi_haptics *haptics)
{
	struct device_node *child;
	u32 value;
	int length;
	int ret;

	child = of_get_next_available_child(haptics->dev->of_node, NULL);
	if (!child) {
		haptics->waveform[0] = 0x7e;
		haptics->waveform[1] = 0x3e;
		haptics->waveform_length = 2;
		return 0;
	}

	length = of_property_count_elems_of_size(child, "qcom,wf-pattern",
					       sizeof(u8));
	if (length <= 0) {
		ret = length < 0 ? length : -EINVAL;
		of_node_put(child);
		return ret;
	}

	haptics->waveform_length = min(length, HAP_WAVEFORM_BUFFER_MAX);
	ret = of_property_read_u8_array(child, "qcom,wf-pattern",
					      haptics->waveform,
					      haptics->waveform_length);
	if (ret) {
		of_node_put(child);
		return ret;
	}

	if (!of_property_read_u32(child, "qcom,wf-play-rate-us", &value))
		haptics->play_rate_us = min_t(u32, value, HAP_PLAY_RATE_US_MAX);
	of_property_read_u8_array(child, "qcom,wf-brake-pattern",
				  haptics->brake, HAP_BRAKE_PATTERN_MAX);
	of_node_put(child);

	return 0;
}

static int qcom_spmi_haptics_parse_dt(struct platform_device *pdev,
					      struct qcom_spmi_haptics *haptics)
{
	const char *string;
	u32 value;
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, "reg", &value);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to read reg\n");
	haptics->base = value;

	haptics->actuator_type = QCOM_HAPTICS_LRA;
	if (!of_property_read_string(pdev->dev.of_node, "qcom,actuator-type", &string)) {
		if (!strcmp(string, "erm"))
			haptics->actuator_type = QCOM_HAPTICS_ERM;
		else if (strcmp(string, "lra"))
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid actuator type\n");
	}

	haptics->lra_shape = QCOM_HAPTICS_LRA_SINE;
	if (!of_property_read_string(pdev->dev.of_node,
				     "qcom,lra-resonance-sig-shape", &string)) {
		if (!strcmp(string, "square"))
			haptics->lra_shape = QCOM_HAPTICS_LRA_SQUARE;
		else if (strcmp(string, "sine"))
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid LRA resonance signal shape\n");
	}

	haptics->auto_res_mode = QCOM_HAPTICS_AUTO_RES_ZXD;
	if (!of_property_read_string(pdev->dev.of_node,
				     "qcom,lra-auto-resonance-mode", &string)) {
		if (!strcmp(string, "qwd"))
			haptics->auto_res_mode = QCOM_HAPTICS_AUTO_RES_QWD;
		else if (strcmp(string, "zxd"))
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "invalid LRA auto resonance mode\n");
	}

	haptics->vmax_mv = HAP_VMAX_MV_DEFAULT;
	if (!of_property_read_u32(pdev->dev.of_node, "qcom,vmax-mv", &value))
		haptics->vmax_mv = min_t(u32, value, HAP_VMAX_MV_MAX);

	haptics->play_rate_us = HAP_PLAY_RATE_US_DEFAULT;
	if (!of_property_read_u32(pdev->dev.of_node, "qcom,play-rate-us", &value))
		haptics->play_rate_us = min_t(u32, value, HAP_PLAY_RATE_US_MAX);

	haptics->brake[0] = 2;
	haptics->brake[1] = 1;

	return qcom_spmi_haptics_parse_waveform(haptics);
}

static int qcom_spmi_haptics_probe(struct platform_device *pdev)
{
	struct qcom_spmi_haptics *haptics;
	struct input_dev *input_dev;
	int ret;

	haptics = devm_kzalloc(&pdev->dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->dev = &pdev->dev;
	haptics->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!haptics->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "failed to get parent regmap\n");

	ret = devm_mutex_init(&pdev->dev, &haptics->lock);
	if (ret)
		return ret;

	ret = qcom_spmi_haptics_parse_dt(pdev, haptics);
	if (ret)
		return ret;

	INIT_WORK(&haptics->work, qcom_spmi_haptics_work);
	platform_set_drvdata(pdev, haptics);

	haptics->play_irq = platform_get_irq_byname_optional(pdev, "hap-play-irq");
	if (haptics->play_irq < 0)
		haptics->play_irq = platform_get_irq_byname_optional(pdev, "play");
	if (haptics->play_irq >= 0) {
		ret = devm_request_threaded_irq(&pdev->dev, haptics->play_irq,
					       NULL, qcom_spmi_haptics_play_irq,
					       IRQF_ONESHOT, "qcom-haptics-play",
					       haptics);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to request play IRQ\n");
	}

	haptics->sc_irq = platform_get_irq_byname_optional(pdev, "hap-sc-irq");
	if (haptics->sc_irq < 0)
		haptics->sc_irq = platform_get_irq_byname_optional(pdev, "short");
	if (haptics->sc_irq >= 0) {
		ret = devm_request_threaded_irq(&pdev->dev, haptics->sc_irq,
					       NULL, qcom_spmi_haptics_sc_irq,
					       IRQF_ONESHOT, "qcom-haptics-short",
					       haptics);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to request short-circuit IRQ\n");
	}

	ret = qcom_spmi_haptics_hw_init(haptics);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to initialize haptics hardware\n");

	input_dev = devm_input_allocate_device(&pdev->dev);
	if (!input_dev)
		return -ENOMEM;

	haptics->input_dev = input_dev;
	input_dev->name = "qcom-spmi-haptics";
	input_dev->id.version = 1;
	input_dev->close = qcom_spmi_haptics_close;
	input_set_drvdata(input_dev, haptics);
	input_set_capability(input_dev, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(input_dev, NULL,
				       qcom_spmi_haptics_play_effect);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to create FF device\n");

	return input_register_device(input_dev);
}

static void qcom_spmi_haptics_remove(struct platform_device *pdev)
{
	struct qcom_spmi_haptics *haptics = platform_get_drvdata(pdev);

	cancel_work_sync(&haptics->work);
	qcom_spmi_haptics_play(haptics, false);
	qcom_spmi_haptics_module_enable(haptics, false);
}

static int qcom_spmi_haptics_suspend(struct device *dev)
{
	struct qcom_spmi_haptics *haptics = dev_get_drvdata(dev);

	cancel_work_sync(&haptics->work);
	qcom_spmi_haptics_play(haptics, false);
	qcom_spmi_haptics_module_enable(haptics, false);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(qcom_spmi_haptics_pm_ops,
				       qcom_spmi_haptics_suspend, NULL);

static const struct of_device_id qcom_spmi_haptics_match[] = {
	{ .compatible = "qcom,haptics" },
	{ .compatible = "qcom,pm8150b-haptics" },
	{ .compatible = "qcom,spmi-haptics" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_spmi_haptics_match);

static struct platform_driver qcom_spmi_haptics_driver = {
	.probe = qcom_spmi_haptics_probe,
	.remove = qcom_spmi_haptics_remove,
	.driver = {
		.name = "qcom-spmi-haptics",
		.pm = pm_sleep_ptr(&qcom_spmi_haptics_pm_ops),
		.of_match_table = qcom_spmi_haptics_match,
	},
};
module_platform_driver(qcom_spmi_haptics_driver);

MODULE_DESCRIPTION("Qualcomm SPMI haptics driver");
MODULE_LICENSE("GPL");
