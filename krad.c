/*
 * Experimental module for using a geiger counter as a hardware RNG
 *
 * Author:
 *      Stefan Wendler (devnull@kaltpost.de)
 *      Brendan Whitfield (me@brendan-w.com)
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
#include <linux/time.h>
#include <linux/hw_random.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/circ_buf.h>

#define DEBUG 1

/* Define a GPIO for the Geiger counter */
static int geiger_pulse_pin = 3;

/* the assigned IRQ for the geiger pulse pin */
static int geiger_irq = -1;

//circular buffer of random pulse times
#define BUFFER_SIZE (PAGE_SIZE / sizeof(struct timespec))
static struct timespec* buffer;
static int buffer_head = 0;
static int buffer_tail = 0;

DEFINE_SPINLOCK(producer_lock); //lock for the ISR, not that it should need one...
DEFINE_SPINLOCK(consumer_lock); //lock for hwrng API



static int geiger_data_present(struct hwrng* rng, int wait)
{
    int head;
    int tail;
    int size;

    spin_lock(&consumer_lock);
    head = smp_load_acquire(&buffer_head);
    tail = buffer_tail;
    spin_unlock(&consumer_lock);

    size = CIRC_CNT(head, tail, BUFFER_SIZE) * sizeof(struct timespec);

    printk(KERN_INFO "krad: geiger_data_present (%d bytes)", size);

    return size;
}

//the old hwrng API
static int geiger_data_read(struct hwrng* rng, u32 *data)
{
    int bytes = 0;
    int head;
    int tail;

    printk(KERN_INFO "krad: geiger_data_read called\n");

    spin_lock(&consumer_lock);

    head = smp_load_acquire(&buffer_head);
    tail = buffer_tail;

    if(CIRC_CNT(head, tail, BUFFER_SIZE) >= 1)
    {
        *data = (u32) buffer[tail].tv_nsec;
	smp_store_release(&buffer_tail, (tail + 1) & (BUFFER_SIZE - 1));
        bytes = 4;
    }

    spin_unlock(&consumer_lock);
    return bytes;
}

//the new hwrng API
static int geiger_read(struct hwrng* rng, void* data, size_t max, bool wait)
{
    int head;
    int tail;
    size_t p;
    size_t pulses_given;

    printk(KERN_INFO "krad: geiger_read called\n");

    spin_lock(&consumer_lock);

    head = smp_load_acquire(&buffer_head);
    tail = buffer_tail;

    //figure out how much we can give them
    pulses_given = min((size_t) max / sizeof(struct timespec),      //pulses wanted
                       (size_t) CIRC_CNT(head, tail, BUFFER_SIZE)); //pulses we have

    if(!pulses_given)
    {
        printk(KERN_INFO "krad: %s was called with max bytes smaller than the storage type\n", __func__);
    }

    for(p = 0; p < pulses_given; p++)
    {
        #ifdef DEBUG
        printk(KERN_INFO "krad: dispensed pulse: %ld seconds %ld nanoseconds \n", buffer[tail].tv_sec, buffer[tail].tv_nsec);
        #endif

        ((struct timespec*) data)[p] = buffer[tail];
        smp_store_release(&buffer_tail, (tail + 1) & (BUFFER_SIZE - 1));
    }

    spin_unlock(&consumer_lock);

    return pulses_given * sizeof(struct timespec);
}


static struct hwrng geiger_rng = {
    "Geiger Counter",
    NULL,
    NULL,
    geiger_data_present,
    geiger_data_read,
    geiger_read,
    0,
    32
};


/*
 * The interrupt service routine called on geiger pulses
 */
static irqreturn_t geiger_isr(int irq, void *data)
{
    if(irq == geiger_irq)
    {
        struct timespec t = CURRENT_TIME;
        int head;
        int tail;

        #ifdef DEBUG
        printk(KERN_INFO "krad: acquired pulse: %ld seconds %ld nanoseconds \n", t.tv_sec, t.tv_nsec);
        #endif

        spin_lock(&producer_lock);

        head = buffer_head;
        tail = ACCESS_ONCE(buffer_tail);

        if(CIRC_SPACE(head, tail, BUFFER_SIZE) >= 1)
        {
            buffer[head] = t;
            smp_store_release(&buffer_head, (head + 1) & (BUFFER_SIZE - 1));
        }

        spin_unlock(&producer_lock);
    }

    return IRQ_HANDLED;
}

/*
 * Module init function
 */
static int __init krad_init(void)
{
    int ret = 0;

    //allocate a single page for our circular buffer
    buffer = (struct timespec*) __get_free_page(GFP_KERNEL);

    if(!buffer)
    {
        printk(KERN_ERR "krad: Not enough memory for buffer\n");
        return ENOMEM;
    }

    // register Geiger pulse gpio
    ret = gpio_request_one(geiger_pulse_pin, GPIOF_IN, "Geiger Pulse");

    if(ret)
    {
        printk(KERN_ERR "krad: Unable to request GPIO for the Geiger Counter: %d\n", ret);
        goto fail1;
    }

    ret = gpio_to_irq(geiger_pulse_pin);

    if(ret < 0)
    {
        printk(KERN_ERR "krad: Unable to request IRQ: %d\n", ret);
        goto fail2;
    }

    geiger_irq = ret;

    ret = request_irq(geiger_irq, geiger_isr, IRQF_TRIGGER_RISING, "krad#geiger", NULL);

    if(ret)
    {
        printk(KERN_ERR "krad: Unable to request IRQ: %d\n", ret);
        goto fail2;
    }

    ret = hwrng_register(&geiger_rng);

    if(ret)
    {
        printk(KERN_ERR "krad: Unable to register hardware RNG device: %d\n", ret);
        goto fail3;
    }

    printk(KERN_INFO "krad: started (buffer size %lu pulses)\n", BUFFER_SIZE);

    // finished successfully
    return 0;


    // failure cases
fail3:
    free_irq(geiger_irq, NULL);
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
    // unregister the hwrng
    hwrng_unregister(&geiger_rng);

    // free irqs
    free_irq(geiger_irq, NULL);

    // unregister
    gpio_free(geiger_pulse_pin);

    //release our buffer memory
    free_page((unsigned long) buffer);

    printk(KERN_INFO "krad: stopped\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Brendan Whitfield");
MODULE_DESCRIPTION("Module for using a geiger counter as a hardware RNG");

module_init(krad_init);
module_exit(krad_exit);
