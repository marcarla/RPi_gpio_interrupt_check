/**************************************************************************
 *    irqlevel.c - A module to check the behaviour of the level           *
 *                 triggered gpio interrupt system                        *
 *                                                                        *
 *  by:           Marcello Carla'                                         *
 *  at:           Department of Physics - University of Florence, Italy   *
 *  email:        carla@fi.infn.it                                        *
 *                                                                        *
 *    A 1 kHz square wave is sent to pin <n>. High and low level          *
 *    interrupts should alternate every <cadence> +/- <tolerance> us.     *
 *                                                                        *
 *    Install with: insmod irqlevel.ko pins=<pin 1>,<pin 2>....<pin 8>.   *
 *    Default is pins=16,21                                               *
 *                                                                        *
 *    Start the test with: cat /dev/irqflow/pin<n>. Every <setsize>       *
 *    events a statistic summary is printed. Bad events are logged in     *
 *    /var/log/kern.log. Stop the test with Ctrl-c.                       *
 *                                                                        *
 *    Module parameters, adjustable at insmod or on the fly, are:         *
 *        setsize      [10000 events] frequency of the statistic summary  *
 *        cadence      [500 us] expected interval from interrupt to       *
 *                              interrupt                                 *
 *        tolerance    [100 us] allowed skew in interrupt interval        *
 *                                                                        *
 *                                                                        *
 *  Copyright: (2023) Marcello Carla'                                     *
 *  This program is free software; you can redistribute it and/or modify  *
 *  it under the terms of the GNU General Public License as published by  *
 *  the Free Software Foundation; either version 2 of the License, or     *
 *  (at your option) any later version.                                   *
 *                                                                        *
 **************************************************************************/
    
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>       /* class functions */
#include <linux/sched.h>        /* fops functions */
#include <linux/fs.h>           /* fops - chrdev_region */
#include <linux/interrupt.h>    /* interrupt facility */
#include <linux/cdev.h>         /* struct cdev */
#include <linux/uaccess.h>      /* copy_to_user */
#include <linux/slab.h>         /* kmalloc */
#include <linux/delay.h>

#include <linux/gpio.h>

MODULE_LICENSE("GPL v2");

/* constants */

#define NAME "irqlevel"
#define HERE  NAME, (char *) __FUNCTION__
#define BASE_MINOR 0
#define MAXPIN 8

/* user parameters */

static int debug = 0;
module_param (debug, int, S_IRUGO | S_IWUSR);
static int setsize = 10000;
module_param (setsize, int, S_IRUGO | S_IWUSR);
static int cadence = 500;
module_param (cadence, int, S_IRUGO | S_IWUSR);
static int tolerance = 100;
module_param (tolerance, int, S_IRUGO | S_IWUSR);

/* global variables */

static dev_t device=0;                 /* base device number */
static struct cdev cdev;
static int cdev_flag=0;
static struct class * dev_class=NULL;
static struct device * dev_device[MAXPIN];

/* default interrupt pins are #16 and #21; up to 8 pins can be declared
                      at load time with pins=<pin0>,<pin1>, .... ,<pin7> */

static int npins=2;
static ushort pins[MAXPIN]={16,21};
module_param_array (pins, ushort, &npins, S_IRUGO);

struct pin_data {
        int irq;                  /* irq number associated with gpio line */
        int pin;                  /* gpio line number */
        wait_queue_head_t queue;
        struct gpio_desc *gpio;   /* gpio descriptor associated to gpio line */
        struct timespec64 last;   /* last interrupt time */
        struct timespec64 first;  /* first interrupt time of the set */
        long count, bad, savebad; /* events and bad events count */
        int val;                  /* last line value */
        long usdiff;              /* last time interval */
        long set_time;            /* us lenght of the set */
        int done;                 /* processed a set of <setsize> interrupts */
        int setsize;              /* wake-up read() after so many interrupts */
        long tmax, tmin;          /* time limits of interrupt interval */
        int level;                
};

/*  logs can be switched on/off with
                      "echo 1/0 > /sys/modules/irqflow/parameters/debug"  */

#define dbg_printk(level,frm,...) if (debug>=level)	\
               printk(KERN_INFO "%s:%s - " frm, HERE, ## __VA_ARGS__ )

/*
 *  compute microsec time difference between two timespec64 times
 */

inline long usec(struct timespec64 after, struct timespec64 before) {
        long udiff;
        struct timespec64 diff;
        diff = timespec64_sub(after, before);
        udiff = diff.tv_nsec/1000 + diff.tv_sec*1000000;
        return udiff;
}

/*
 *    interrupt service routine
 */

#define Event ((struct pin_data *) arg)

irqreturn_t irq_service(int irq, void * arg) {
        int val;
        long usdiff;
        struct timespec64 now;

       /* acquire event and preset for next interrupy level */

        val = gpiod_get_value(Event->gpio);
        ktime_get_ts64 (&now);
        irq_set_irq_type (Event->irq, Event->level ? IRQ_TYPE_LEVEL_LOW : IRQ_TYPE_LEVEL_HIGH);

        /* check event */

        usdiff = usec (now, Event->last);

        if (Event->count <= 0) {
                dbg_printk (0, "irq %d:%d - val %d -> %d : %d  after %ld / %ld us  bad ev.: %ld:%ld\n",
                        Event->pin, Event->irq, Event->val, val, Event->level, usdiff, Event->usdiff,
                        Event->bad, Event->count);
        }

        if (Event->count > 0 && (usdiff > Event->tmax || usdiff < Event->tmin ||
                                        (val != Event->level))) {

                Event->bad++;
                dbg_printk (0, "irq %d:%d - val %d -> %d : %d  after %ld / %ld us  bad ev.: %ld:%ld\n",
                        Event->pin, Event->irq, Event->val, val, Event->level, usdiff, Event->usdiff,
                        Event->bad, Event->count);
        }

        /* save values from this event */

        Event->val = val;
        Event->usdiff = usdiff;
        Event->last = now;

        /* end of a set of <setsize> events - save results for read() */

        if (Event->count == 0) Event->first = now;
        if (Event->count++ == setsize) {
                Event->done = 1;
                Event->savebad = Event->bad;
                Event->bad = 0;
                Event->set_time = usec(now, Event->first);
                Event->first = now;
                Event->count = 1;
                wake_up_interruptible(&Event->queue);
        }

        Event->level ^= 1;

        return IRQ_HANDLED;
}

/*
 *    read
 */

ssize_t read (struct file *filp, char *buf,
              const size_t count, loff_t *ppos) {

        struct pin_data * events = filp->private_data;
        int retval;
        char stat[80];
        int leng;
        struct tm date;
        time64_t now;

        events->done = 0;
        retval = wait_event_interruptible (events->queue, events->done);
        if (retval) return -ERESTARTSYS;

        now = ktime_get_real_seconds();
        time64_to_tm(now, 0, &date);
        leng = scnprintf (stat, 80, "%ld-%.02d-%.02d %.02d:%.02d:%.02d Events:"
                                    " %d in %ld usec on pin %d. Bad events: %ld\n",
               date.tm_year+1900, date.tm_mon+1, date.tm_mday, date.tm_hour, date.tm_min, date.tm_sec,
               events->setsize, events->set_time, events->pin, events->savebad);

        leng = leng > count ? count : leng;
        retval = copy_to_user (buf, stat, leng);

        return leng - retval;
}

/*
 * write
 */

ssize_t write (struct file *filp, const char *user_buf,
               size_t count, loff_t *ppos) {
               
        dbg_printk (0, "Write request for %zu bytes - but this is a nop.\n", count);
                
        return count;
}


void resource_release (struct pin_data * event) {

        if (event->irq) {
               disable_irq (event->irq); /* disable irq and wait for pending actions */
               free_irq(event->irq, event);
        }
        if (event->gpio) gpiod_put (event->gpio);
        if (event->pin) gpio_free (event->pin);
        kfree (event);
}

/*
 *    release
 */

int release (struct inode *inode, struct file *filp) {
        struct pin_data * event = filp->private_data;

        dbg_printk (0, "close request for pin %d\n", event->pin);
        resource_release (event);

        return 0;
}

/*
 *    open
 */

int open (struct inode *inode, struct file *filp) {

        int status;
        struct pin_data * event;
        int minor = MINOR(inode->i_rdev);

        dbg_printk (0, "open device %d:%d pin %d\n",
                MAJOR(inode->i_rdev), minor, pins[minor]);

        /* create data structure for events on this pin */

        event = kzalloc (sizeof(struct pin_data), GFP_KERNEL);
        if (event == NULL) {
                dbg_printk (0, "Unable to obtain memory\n");
                return -ENOMEM;
        }
        filp->private_data = event;     /* save for read() and release() */

        /* allocate gpio - another process request fails here */

        status = gpio_request_one (pins[minor], GPIOF_DIR_IN, NULL);
        if (status) {
                dbg_printk(0, "Unable to obtain gpio %d.\n", pins[minor]);
                goto failure;
        } else {
                event->pin = pins[minor];
        }

        /* get gpio descriptor */

        event->gpio = gpio_to_desc(pins[minor]);
        if (event->gpio == NULL) {
		        dbg_printk(0, "Unable to obtain gpio descriptor %d.\n", pins[minor]);
                goto failure;
	    }

        init_waitqueue_head (&event->queue);

        event->irq = gpiod_to_irq(event->gpio);

        /* everything is ready - register interrupt routine */

        event->count = -3;  /* ignore first events after irq line activation */
        event->val = 0;
        event->setsize = setsize;
        event->tmax = cadence + tolerance;
        event->tmin = cadence - tolerance;
        event->level = 1;
        if (request_threaded_irq(event->irq, irq_service, NULL,
                        IRQF_TRIGGER_HIGH, NAME, event)) {
		        dbg_printk(0, "can't register IRQ %d\n", event->irq);
                event->irq = 0;
                goto failure;
        }
       
        dbg_printk(1, "Registered IRQ %d for pin %d.\n", event->irq, event->pin);

        return 0;

failure:
        resource_release (event);
        return -1;
}

/*
 *    fops for the irqflow devices
 */

static struct file_operations fops= {
        .owner = THIS_MODULE,
        .open = open,
        .release = release,
        .read = read,
        .write = write,
};

/*
 *      exit - cleanup and module removal
 */

static void mod_exit (void) {
        int j, major;

        dbg_printk (0, "Unloading module.\n");

        major = MAJOR(device);
        for ( j=0 ; j<npins ; j++ ) {
                if (dev_device[j]) device_destroy (dev_class,
                                            MKDEV(major, j + BASE_MINOR));
        }
        if (dev_class) class_destroy (dev_class);
        if (cdev_flag) cdev_del (&cdev);
        if (device) unregister_chrdev_region(device, npins);
}

/*
 *      init - module initialization: create a device /dev/irqflow/pin<gpio number>
 *             for each requested pin. Pin management deferred to the open() request.
 */

static int mod_init (void) {

        long status=0;
        int j, major;

        /* obtain major device number or exit */

        status = alloc_chrdev_region (&device, BASE_MINOR, npins, NAME);
        if (status < 0) {
                dbg_printk (0, "can't get major\n");
                return status;
        }
        major = MAJOR(device);
        dbg_printk (0, "major is %d\n", major);

        /* create and register the device */

        cdev_init(&cdev, &fops);
        cdev.owner = THIS_MODULE;

        status = cdev_add (&cdev, device, npins);
        if (status) {
                dbg_printk (0, "can't register device %d %ld\n", device, status);
                goto failure;
        }
        cdev_flag = 1;
        dbg_printk (0, "registered device: %d %p %p\n", device, &fops, &cdev);

        /* create the /dev/<...> node */

        dev_class = class_create (THIS_MODULE, NAME);
        if (IS_ERR(dev_class)) {
                status = (long) dev_class;
                dev_class = NULL;
                dbg_printk (0, "class_create failed\n");
                goto failure;
        }
        for ( j=0 ; j<npins ; j++ ) {
                dev_device[j] = device_create(dev_class, NULL,
                           MKDEV(major, j + BASE_MINOR), NULL, NAME "/pin%d", pins[j]);

                dbg_printk (0, "created device %d %p\n", j, dev_device[j]);

                if (IS_ERR(dev_device[j])) {
                        dbg_printk (0, "create of device %d failed\n", j);
                        status = (long) dev_device[j];
                        dev_device[j] = NULL;
                        goto failure;
                }
        }

        dbg_printk (0, "installed by \"%s\" (pid %i) at %p\n", current->comm, current->pid, current);

        return 0;

failure:
        dbg_printk (0, "Module load failed.\n");
        mod_exit();
        return status;
}

module_init (mod_init);
module_exit (mod_exit);

