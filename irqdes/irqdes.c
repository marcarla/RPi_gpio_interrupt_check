/**************************************************************************
 *  irqdes - A module to study the behaviour of the gpio interrupt        *
 *           system with enable/disable_irq() or irq_set_irq_type()       *
 *                                                                        *
 *  by:           (2023) Marcello Carla'                                  *
 *  at:           Department of Physics - University of Florence, Italy   *
 *  email:        carla@fi.infn.it                                        *                                    *
 *                                                                        *
 *    An irq line is sequentially disabled and re-enabled after a time;   *
 *    events (rising and falling edges) are sent both while disabled or   *
 *    enabled and the triggered interrupts are reported.                  *
 *                                                                        *
 *    Install with: insmod irqdes.ko pins=<pin 1>,<pin 2>....<pin 8>      *
 *    Pins are in pairs: <irq pin>,<drive pin>,..., up to 4 pairs can     *
 *    be given. Default: 16,21. Short each pair with a jumper.            *
 *                                                                        *
 *    Start the test with: "cat /dev/irqdes/pin<n>". Nothing is printed,  *
 *    actions and events are logged in /var/log/kern.log.                 *
 *                                                                        *
 *    The following parameters can be djusted at insmod, or changed       *
 *    on the fly (effective at next "cat /dev/irq...."):                  *
 *       value    [0]  first level to be sent                             *
 *       cadence  [10] ms interval before and after each event/action     *
 *       cycles   [5]  how many disable/enable cycles                     *
 *       ni       [5]  events before first enable                         *
 *       enab     [5]  events while enabled                               *
 *       disab    [4]  events while disabled                              *
 *       mode     [0]  0: use enable/disable_irq()                        *
 *                     1: use irq_set_irq_type()                          *
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
#include <linux/irq.h>
#include <linux/cdev.h>         /* struct cdev */
#include <linux/slab.h>         /* kmalloc */
#include <linux/delay.h>

#include <linux/gpio.h>

MODULE_LICENSE("GPL v2");

/* constants */

#define NAME "irqdes"
#define HERE  NAME, (char *) __FUNCTION__
#define BASE_MINOR 0
#define MAXPIN 8

/* global variables */

static int debug = 0;
module_param (debug, int, S_IRUGO | S_IWUSR);

static int value = 0;
module_param (value, int, S_IRUGO | S_IWUSR);
static int cadence = 10;
module_param (cadence, int, S_IRUGO | S_IWUSR);
static int cycles = 5;
module_param (cycles, int, S_IRUGO | S_IWUSR);
static int ni = 5;
module_param (ni, int, S_IRUGO | S_IWUSR);
static int enab = 5;
module_param (enab, int, S_IRUGO | S_IWUSR);
static int disab = 4;
module_param (disab, int, S_IRUGO | S_IWUSR);
static int mode = 0;
module_param (mode, int, S_IRUGO | S_IWUSR);

static dev_t device=0;     /* base device number */
static struct cdev cdev;
static int cdev_flag=0;
static struct class * dev_class=NULL;
static struct device * dev_device[MAXPIN];

/* default pins are irq:16  drive:21 */

static int npins=2;
static ushort pins[MAXPIN]={16,21};
module_param_array (pins, ushort, &npins, S_IRUGO);

struct pin_data {
        int irq;                  /* irq number associated with gpio line */
        int irqpin;               /* gpio interrupt line number */
        int drvpin;               /* gpio drive line number */
        struct gpio_desc * igpio; /* gpio descriptor associated to irq line */
        struct gpio_desc * dgpio; /* gpio descriptor associated to drive line */
        int val;                  /* next value to be sent to drive line */
        int cadence;              /* time delay before and after actions */
};

#define dbg_printk(level,frm,...) if (debug>=level)	\
               printk(KERN_INFO "%s:%s - " frm, HERE, ## __VA_ARGS__ )

/*
 *    interrupt service routine
 */

#define Event ((struct pin_data *) arg)
irqreturn_t irq_service(int irq, void * arg) {

        dbg_printk (0, "irq %d:%d - val %d -> %d\n", Event->irqpin, Event->irq,
                gpiod_get_value(Event->dgpio), gpiod_get_value(Event->igpio));

        return IRQ_HANDLED;
}

/*
 *  toggle the irq line level (connect with a jumper drive gpio to irq gpio)
 */

void edges (struct pin_data * events, int nev) {
        int j;
        for ( j=0 ; j<nev; j++ ) {
                dbg_printk (0, "gpio %d - sending %d\n", events->irqpin, events->val);
                gpiod_set_value(events->dgpio, events->val);
                events->val ^= 1;
                msleep (events->cadence);
         }
}

/*
 *    read
 */

ssize_t read (struct file *filp, char *buf,
              const size_t count, loff_t *ppos) {

        struct pin_data * events = filp->private_data;
        int j;
        int nc = cycles;
        int mi = ni;
        int me = enab;
        int md = disab;
        int sel = mode; 
        events->val = value;
        events->cadence = cadence;

        dbg_printk (0, "gpio %d:%d - Disabling irq and starting test.\n",
                events->irqpin, events->drvpin);

        if (sel) irq_set_irq_type (events->irq, IRQ_TYPE_NONE);
        else disable_irq (events->irq);

        msleep (events->cadence);
        edges(events, mi);

        for ( j=0 ; j<nc ; j++ ) {
                dbg_printk (0, "gpio %d - enabling irq.\n", events->irqpin);

                if (sel) irq_set_irq_type (events->irq, IRQ_TYPE_EDGE_BOTH);
                else enable_irq (events->irq);

                msleep (events->cadence);
                edges(events, me);
                dbg_printk (0, "gpio %d - disabling irq.\n", events->irqpin);

                if (sel) irq_set_irq_type (events->irq, IRQ_TYPE_NONE);
                else disable_irq (events->irq);

                msleep (events->cadence);
                edges(events, md);
        }
        return -ERESTARTSYS;
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

        if (event->dgpio) gpiod_put (event->dgpio);
        if (event->drvpin) gpio_free (event->drvpin);

        if (event->igpio) gpiod_put (event->igpio);
        if (event->irqpin) gpio_free (event->irqpin);
        kfree (event);
}

/*
 *    release
 */

int release (struct inode *inode, struct file *filp) {
        struct pin_data * event = filp->private_data;

        dbg_printk (0, "close request for pins %d:%d\n", event->irqpin, event->drvpin);
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
        int irqpin = pins[minor*2];
        int drvpin = pins[minor*2+1];
        
        dbg_printk (0, "device %d:%d pin %d\n",
                MAJOR(inode->i_rdev), minor, pins[minor*2]);

        /* create data structure for events on this pin */

        event = kzalloc (sizeof(struct pin_data), GFP_KERNEL);
        if (event == NULL) {
                dbg_printk (0, "Unable to obtain memory\n");
                return -ENOMEM;
        }
        filp->private_data = event;     /* save for read() and release() */

        /* allocate gpios - request from another process for same gpio fails here */

        status = gpio_request_one (irqpin, GPIOF_DIR_IN, NULL);   /* allocate gpio for irq */
        if (status) {
                dbg_printk(0, "Unable to obtain gpio %d.\n", irqpin);
                goto failure;
        } else {
                event->irqpin = irqpin;
        }

        event->igpio = gpio_to_desc(irqpin);               /* get the gpio descriptor for irq */
        if (event->igpio == NULL) {
		        dbg_printk(0, "Unable to obtain gpio descriptor %d.\n", pins[minor]);
                goto failure;
	    }

        event->irq = gpiod_to_irq(event->igpio);

        status = gpio_request_one (drvpin, GPIOF_DIR_OUT, NULL);   /* allocate gpio for drive */
        if (status) {
                dbg_printk(0, "Unable to obtain gpio %d.\n", drvpin);
                goto failure;
        } else {
                event->drvpin = drvpin;
        }

        event->dgpio = gpio_to_desc(drvpin);               /* get the gpio descriptor for drive */
        if (event->dgpio == NULL) {
		        dbg_printk(0, "Unable to obtain gpio descriptor %d.\n", pins[minor]);
                goto failure;
	    }

        /* everything is ready - register the interrupt routine keeping interrupt disabled */

        if (request_threaded_irq(event->irq, irq_service, NULL,
                    /* IRQF_NO_AUTOEN | */ IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, NAME, event)) {
		        dbg_printk(0, "can't register IRQ %d\n", event->irq);
                event->irq = 0;
                goto failure;
        }
       
        dbg_printk(0, "Registered IRQ %d for pin %d.\n", event->irq, event->irqpin);

        return 0;

failure:
        resource_release (event);
        return -1;
}

/*
 *    fops for the irqtest devices
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
        int ndev = npins/2;

        dbg_printk (0, "Unloading module.\n");

        major = MAJOR(device);
        for ( j=0 ; j<ndev ; j++ ) {
                if (dev_device[j]) device_destroy (dev_class,
                                            MKDEV(major, j + BASE_MINOR));
        }
        if (dev_class) class_destroy (dev_class);
        if (cdev_flag) cdev_del (&cdev);
        if (device) unregister_chrdev_region(device, npins);
}

/*
 *      init - module initialization: create a device /dev/irqdes/pin<gpio number>
 *             for each requested irq pin. Pin management deferred to the open() request.
 */

static int mod_init (void) {

        long status=0;
        int j, major;
        int ndev = npins/2;

        /* obtain major device number or exit */

        status = alloc_chrdev_region (&device, BASE_MINOR, ndev, NAME);
        if (status < 0) {
                dbg_printk (0, "can't get major\n");
                return status;
        }
        major = MAJOR(device);
        dbg_printk (0, "major is %d\n", major);

        /* create and register the device */

        cdev_init(&cdev, &fops);
        cdev.owner = THIS_MODULE;

        status = cdev_add (&cdev, device, ndev);
        if (status) {
                dbg_printk (0, "can't register device %d %ld\n", device, status);
                goto failure;
        }
        cdev_flag = 1;

        /* create the /dev/<...> node */

        dev_class = class_create (THIS_MODULE, NAME);
        if (IS_ERR(dev_class)) {
                status = (long) dev_class;
                dev_class = NULL;
                dbg_printk (0, "class_create failed\n");
                goto failure;
        }
        for ( j=0 ; j<ndev ; j++ ) {
                dev_device[j] = device_create(dev_class, NULL,
                           MKDEV(major, j + BASE_MINOR), NULL, NAME "/pin%d", pins[2*j]);

                dbg_printk (0, "created device /dev/" NAME "/pin%d - minor %d\n", pins[2*j], j);

                if (IS_ERR(dev_device[j])) {
                        dbg_printk (0, "create of device %d failed\n", j);
                        status = (long) dev_device[j];
                        dev_device[j] = NULL;
                        goto failure;
                }
        }

        dbg_printk (0, "installed by \"%s\"\n", current->comm);

        return 0;

failure:
        dbg_printk (0, "Module load failed.\n");
        mod_exit();
        return status;
}

module_init (mod_init);
module_exit (mod_exit);


