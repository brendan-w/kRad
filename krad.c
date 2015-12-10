/*
 * Basic Linux Kernel module using GPIO interrupts.
 *
 * Author:
 * 	Stefan Wendler (devnull@kaltpost.de)
 *      Brendan Whitfield (bcw7044@rit.edu)
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h> 

/* Define GPIOs for BUTTONS */
static int geiger_pulse_pin = 17; // listens for incoming pulses from the gieger counter


/* Later on, the assigned IRQ numbers for the buttons are stored here */
static int geiger_irq = -1;

/*
 * The interrupt service routine called on geiger pulses
 */
static irqreturn_t geiger_isr(int irq, void *data)
{
	if(irq == geiger_irq)
	{
		printk(KERN_INFO "pulse");
	}

	return IRQ_HANDLED;
}

/*
 * Module init function
 */
static int __init krad_init(void)
{
	int ret = 0;

	printk(KERN_INFO "%s\n", __func__);

	// register BUTTON gpios
	ret = gpio_request_one(geiger_pulse_pin, GPIOF_IN, "Geiger Pulse");

	if (ret) {
		printk(KERN_ERR "Unable to request GPIOs for Geiger Counter: %d\n", ret);
		goto fail1;
	}

	//printk(KERN_INFO "Current button1 value: %d\n", gpio_get_value(buttons[0].gpio));
	
	ret = gpio_to_irq(geiger_pulse_pin);

	if(ret < 0) {
		printk(KERN_ERR "Unable to request IRQ: %d\n", ret);
		goto fail2;
	}

	geiger_irq = ret;

	printk(KERN_INFO "Successfully requested Geiger Pulse IRQ # %d\n", geiger_irq);

	ret = request_irq(geiger_irq, geiger_isr, IRQF_TRIGGER_RISING, "gpiomod#button1", NULL);

	if(ret) {
		printk(KERN_ERR "Unable to request IRQ: %d\n", ret);
		goto fail2;
	}


	return 0;

// cleanup what has been setup so far

fail2: 
	gpio_free(geiger_pulse_pin);
fail1:
	return ret;	
}

/**
 * Module exit function
 */
static void __exit krad_exit(void)
{
	printk(KERN_INFO "%s\n", __func__);

	// free irqs
	free_irq(geiger_irq, NULL);
	
	// unregister
	gpio_free(geiger_pulse_pin);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stefan Wendler");
MODULE_DESCRIPTION("Basic Linux Kernel module using GPIO interrupts");

module_init(krad_init);
module_exit(krad_exit);


