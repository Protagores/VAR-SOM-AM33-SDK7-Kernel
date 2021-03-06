/*
 * Driver for watchdog device controlled through GPIO-line
 *
 * Author: 2013, Alexander Shiyan <shc_work@mail.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>

#define SOFT_TIMEOUT_MIN	1
#define SOFT_TIMEOUT_DEF	60
#define SOFT_TIMEOUT_MAX	0xffff

enum {
	HW_ALGO_TOGGLE,
	HW_ALGO_LEVEL,
};

struct gpio_wdt_priv {
	int			gpio;
	bool			active_low;
	bool			state;
	unsigned int		hw_algo;
	unsigned int		hw_margin_ms;
	unsigned long		period;
	unsigned long		last_jiffies;
	struct notifier_block	notifier;
	struct delayed_work	work;
	struct watchdog_device	wdd;
};

static void gpio_wdt_disable(struct gpio_wdt_priv *priv)
{
	gpio_set_value_cansleep(priv->gpio, !priv->active_low);

	/* Put GPIO back to tristate */
	if (priv->hw_algo == HW_ALGO_TOGGLE)
		gpio_direction_input(priv->gpio);
}

static int gpio_wdt_start(struct watchdog_device *wdd)
{
	struct gpio_wdt_priv *priv = watchdog_get_drvdata(wdd);

	priv->state = priv->active_low;
	gpio_direction_output(priv->gpio, priv->state);
	priv->last_jiffies = jiffies;
	schedule_delayed_work(&priv->work, priv->period);

	return 0;
}

static int gpio_wdt_stop(struct watchdog_device *wdd)
{
	struct gpio_wdt_priv *priv = watchdog_get_drvdata(wdd);

	cancel_delayed_work_sync(&priv->work);
	gpio_wdt_disable(priv);

	return 0;
}

static int gpio_wdt_ping(struct watchdog_device *wdd)
{
	struct gpio_wdt_priv *priv = watchdog_get_drvdata(wdd);

	priv->last_jiffies = jiffies;

	return 0;
}

static int gpio_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	wdd->timeout = t;

	return gpio_wdt_ping(wdd);
}

static void gpio_wdt_hwping(struct work_struct *w)
{
	struct gpio_wdt_priv *priv =
		container_of((struct delayed_work *)w, struct gpio_wdt_priv, work);

	if (time_after(jiffies, priv->last_jiffies +
		       msecs_to_jiffies(priv->wdd.timeout * 1000))) {
		dev_crit(priv->wdd.dev, "Timeout expired. System will reboot soon!\n");
		return;
	}

	/* Restart delayed work */
	schedule_delayed_work(&priv->work, priv->period);

	switch (priv->hw_algo) {
	case HW_ALGO_TOGGLE:
		/* Toggle output pin */
		priv->state = !priv->state;
		gpio_set_value_cansleep(priv->gpio, priv->state);
		break;
	case HW_ALGO_LEVEL:
		/* Pulse */
		gpio_set_value_cansleep(priv->gpio, !priv->active_low);
		udelay(1);
		gpio_set_value_cansleep(priv->gpio, priv->active_low);
		break;
	}
}

static int gpio_wdt_notify_sys(struct notifier_block *nb, unsigned long code,
			       void *unused)
{
	struct gpio_wdt_priv *priv = container_of(nb, struct gpio_wdt_priv,
						  notifier);

	cancel_delayed_work_sync(&priv->work);

	switch (code) {
	case SYS_HALT:
	case SYS_POWER_OFF:
		gpio_wdt_disable(priv);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static const struct watchdog_info gpio_wdt_ident = {
	.options	= WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING |
			  WDIOF_SETTIMEOUT,
	.identity	= "GPIO Watchdog",
};

static const struct watchdog_ops gpio_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= gpio_wdt_start,
	.stop		= gpio_wdt_stop,
	.ping		= gpio_wdt_ping,
	.set_timeout	= gpio_wdt_set_timeout,
};

static ssize_t gpio_wdt_show_period_ms(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct gpio_wdt_priv *priv = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", jiffies_to_msecs(priv->period));
}

static ssize_t gpio_wdt_store_period_ms(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct gpio_wdt_priv *priv = dev_get_drvdata(dev);
	char *end;

	unsigned long period_ms = simple_strtoul(buf, &end, 0);
	if (end == buf) {
		return -EINVAL;
	} else if (period_ms > 0 && period_ms < priv->hw_margin_ms) {
		priv->period = msecs_to_jiffies(period_ms);
		return count;
	} else {
		return -EINVAL;
	}
}

static DEVICE_ATTR(period_ms, (S_IRUGO | S_IWUSR),
	gpio_wdt_show_period_ms, gpio_wdt_store_period_ms);

static int gpio_wdt_probe(struct platform_device *pdev)
{
	struct gpio_wdt_priv *priv;
	enum of_gpio_flags flags;
	unsigned int period_ms;
	unsigned long f = 0;
	const char *algo;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->gpio = of_get_gpio_flags(pdev->dev.of_node, 0, &flags);
	if (!gpio_is_valid(priv->gpio))
		return priv->gpio;

	priv->active_low = flags & OF_GPIO_ACTIVE_LOW;

	ret = of_property_read_string(pdev->dev.of_node, "hw_algo", &algo);
	if (ret)
		return ret;
	if (!strncmp(algo, "toggle", 6)) {
		priv->hw_algo = HW_ALGO_TOGGLE;
		f = GPIOF_IN;
	} else if (!strncmp(algo, "level", 5)) {
		priv->hw_algo = HW_ALGO_LEVEL;
		f = priv->active_low ? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW;
	} else {
		return -EINVAL;
	}

	ret = devm_gpio_request_one(&pdev->dev, priv->gpio, f,
				    dev_name(&pdev->dev));
	if (ret)
		return ret;

	ret = of_property_read_u32(pdev->dev.of_node,
				   "hw_margin_ms", &priv->hw_margin_ms);
	if (ret)
		return ret;
	/* Disallow values lower than 2 and higher than 65535 ms */
	if (priv->hw_margin_ms < 2 || priv->hw_margin_ms > 65535)
		return -EINVAL;

	ret = of_property_read_u32(pdev->dev.of_node, "period_ms", &period_ms);
	if (ret) {
		/* Use safe value (1/2 of real timeout) by default */
		priv->period = msecs_to_jiffies(priv->hw_margin_ms / 2);
		period_ms = 0;
	} else if (period_ms > 0 && period_ms < priv->hw_margin_ms) {
		priv->period = msecs_to_jiffies(period_ms);
	} else {
		return -EINVAL;
	}

	watchdog_set_drvdata(&priv->wdd, priv);
	platform_set_drvdata(pdev, priv);

	priv->wdd.info		= &gpio_wdt_ident;
	priv->wdd.ops		= &gpio_wdt_ops;
	priv->wdd.min_timeout	= SOFT_TIMEOUT_MIN;
	priv->wdd.max_timeout	= SOFT_TIMEOUT_MAX;

	if (watchdog_init_timeout(&priv->wdd, 0, &pdev->dev) < 0)
		priv->wdd.timeout = SOFT_TIMEOUT_DEF;

	INIT_DELAYED_WORK(&priv->work, gpio_wdt_hwping);

	ret = watchdog_register_device(&priv->wdd);
	if (ret)
		return ret;

	if (period_ms)
		device_create_file(&pdev->dev, &dev_attr_period_ms);

	priv->notifier.notifier_call = gpio_wdt_notify_sys;
	ret = register_reboot_notifier(&priv->notifier);
	if (ret) {
		device_remove_file(&pdev->dev, &dev_attr_period_ms);
		watchdog_unregister_device(&priv->wdd);
	}

	return ret;
}

static int gpio_wdt_remove(struct platform_device *pdev)
{
	struct gpio_wdt_priv *priv = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&priv->work);
	unregister_reboot_notifier(&priv->notifier);
	device_remove_file(&pdev->dev, &dev_attr_period_ms);
	watchdog_unregister_device(&priv->wdd);

	return 0;
}

static const struct of_device_id gpio_wdt_dt_ids[] = {
	{ .compatible = "linux,wdt-gpio", },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_wdt_dt_ids);

static struct platform_driver gpio_wdt_driver = {
	.driver	= {
		.name		= "gpio-wdt",
		.owner		= THIS_MODULE,
		.of_match_table	= gpio_wdt_dt_ids,
	},
	.probe	= gpio_wdt_probe,
	.remove	= gpio_wdt_remove,
};
module_platform_driver(gpio_wdt_driver);

MODULE_AUTHOR("Alexander Shiyan <shc_work@mail.ru>");
MODULE_DESCRIPTION("GPIO Watchdog");
MODULE_LICENSE("GPL");
