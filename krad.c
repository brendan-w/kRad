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
DEFINE_SPINLOCK(consumer_lock); //lock for hwrng API...



static int geiger_data_present(struct hwrng* rng, int wait)
{
    int bytes = 0;
    int head;
    int tail;
    spin_lock(&consumer_lock);
    head = ACCESS_ONCE(buffer_head);
    tail = ACCESS_ONCE(buffer_tail);
    bytes = CIRC_CNT(head, tail, BUFFER_SIZE) * sizeof(struct timespec);
    spin_unlock(&consumer_lock);
    return bytes;
}

//the new hwrng API
static int geiger_read(struct hwrng* rng, void* data, size_t max, bool wait)
{
    int bytes = 0;
    int head;
    int tail;
    int size;

    spin_lock(&consumer_lock);

    head = smp_load_acquire(&buffer_head);
    tail = buffer_tail;
    size = CIRC_CNT(head, tail, BUFFER_SIZE);

    //ensure that we have new data to give
    if(size > 0)
    {
        if(max > sizeof(struct timespec))
        {
            //how many entries can we give
            int bytes_available = size * sizeof(struct timespec);
            int max_bytes = min((int) max, bytes_available);
            int max_pulses = max_bytes / sizeof(struct timespec); //integer flooring

            struct timespec* output = (struct timespec*) data;

            //give them as much as we can
            while(max_pulses > 0)
            {
                *output = buffer[tail];
                smp_store_release(&buffer_tail, (tail + 1) & (BUFFER_SIZE - 1));

                ++output;
                --max_pulses;
            }

            bytes = max_bytes;
        }
        else
        {
            //else, no implementation for smaller amounts
            printk(KERN_INFO "%s was called with max bytes smaller than the storage type\n", __func__);
        }
    }

    spin_unlock(&consumer_lock);
    return bytes;
}


static struct hwrng geiger_rng = {
    "Geiger Counter",
    NULL,
    NULL,
    geiger_data_present,
    NULL,
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
        printk(KERN_INFO "Geiger %ld seconds %ld nanoseconds \n", t.tv_sec, t.tv_nsec);

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

    printk(KERN_INFO "%s\n", __func__);

    //allocate a single page for our circular buffer
    buffer = (struct timespec*) __get_free_page(GFP_KERNEL);

    if(!buffer)
    {
        printk(KERN_ERR "Not enough memory for buffer\n");
        return ENOMEM;
    }

    printk(KERN_INFO "Allocated buffer for %lu pulses\n", BUFFER_SIZE);

    // register Geiger pulse gpio
    ret = gpio_request_one(geiger_pulse_pin, GPIOF_IN, "Geiger Pulse");

    if(ret)
    {
        printk(KERN_ERR "Unable to request GPIO for the Geiger Counter: %d\n", ret);
        goto fail1;
    }

    ret = gpio_to_irq(geiger_pulse_pin);

    if(ret < 0)
    {
        printk(KERN_ERR "Unable to request IRQ: %d\n", ret);
        goto fail2;
    }

    geiger_irq = ret;

    printk(KERN_INFO "Successfully requested Geiger Pulse IRQ # %d\n", geiger_irq);

    ret = request_irq(geiger_irq, geiger_isr, IRQF_TRIGGER_RISING, "krad#geiger", NULL);

    if(ret)
    {
        printk(KERN_ERR "Unable to request IRQ: %d\n", ret);
        goto fail2;
    }

    ret = hwrng_register(&geiger_rng);

    if(ret)
    {
        printk(KERN_ERR "Unable to register hardware RNG device: %d\n", ret);
        goto fail3;
    }

    printk(KERN_INFO "Successfully registered new hardware RNG device\n");

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
    printk(KERN_INFO "%s\n", __func__);

    // unregister the hwrng
    hwrng_unregister(&geiger_rng);

    // free irqs
    free_irq(geiger_irq, NULL);

    // unregister
    gpio_free(geiger_pulse_pin);

    //release our buffer memory
    free_page((unsigned long) buffer);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Brendan Whitfield");
MODULE_DESCRIPTION("Module for using a geiger counter as a hardware RNG");

module_init(krad_init);
module_exit(krad_exit);
