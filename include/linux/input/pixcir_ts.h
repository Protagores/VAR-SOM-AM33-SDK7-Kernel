#ifndef	_PIXCIR_I2C_TS_H
#define	_PIXCIR_I2C_TS_H

/*
 * Register map
 */
#define PIXCIR_REG_POWER_MODE	51
#define PIXCIR_REG_INT_MODE	52

/*
 * Power modes:
 * active: max scan speed
 * idle: lower scan speed with automatic transition to active on touch
 * halt: datasheet says sleep but this is more like halt as the chip
 *       clocks are cut and it can only be brought out of this mode
 *	 using the RESET pin.
 */
enum pixcir_power_mode {
	PIXCIR_POWER_ACTIVE,
	PIXCIR_POWER_IDLE,
	PIXCIR_POWER_HALT,
};

#define PIXCIR_POWER_MODE_MASK	0x03
#define PIXCIR_POWER_ALLOW_IDLE (1UL << 2)

/*
 * Interrupt modes:
 * periodical: interrupt is asserted periodicaly
 * diff coordinates: interrupt is asserted when coordinates change
 * level on touch: interrupt level asserted during touch
 * pulse on touch: interrupt pulse asserted druing touch
 *
 */
enum pixcir_int_mode {
	PIXCIR_INT_PERIODICAL,
	PIXCIR_INT_DIFF_COORD,
	PIXCIR_INT_LEVEL_TOUCH,
	PIXCIR_INT_PULSE_TOUCH,
};

#define PIXCIR_INT_MODE_MASK	0x03
#define PIXCIR_INT_ENABLE	(1UL << 3)
#define PIXCIR_INT_POL_HIGH	(1UL << 2)

/**
 * struct pixcir_irc_chip_data - chip related data
 * @num_report_ids:     Max number of finger ids reported simultaneously.
 *                      if 0 it means chip doesn't support finger id reporting
 *                      and driver will resort to Type A Multi-Touch reporting.
 */
struct pixcir_i2c_chip_data {
	u8 num_report_ids;
};

struct pixcir_ts_platform_data {
	unsigned int x_size;	/* X axis resolution */
	unsigned int y_size;	/* Y axis resolution */
	int gpio_attb;		/* GPIO connected to ATTB line */
	struct pixcir_i2c_chip_data chip;
};

#endif
