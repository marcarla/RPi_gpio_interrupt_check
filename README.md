
               RPi_gpio_interrupt_check

 Small kernel modules to test the gpio interrupt work
              in a RaspberryPi computer.

irqflow
-------

The irqflow module waits for a stream of interrupt events arriving
at one or more gpio pins with a regular cadence. Each event is checked
for coherency with the flow; every single anomaly is logged into
/var/log/kern.log, a statistics of errors is periodically printed.

A signal generator is required to work with this module.

irqdes
------
The irqdes module tests the behaviour of the inerrupt handling system
when an irq line is in sequence disabled and re-enabled.

------

To compile the modules, in a Raspios virgin system, it should be enough
to complete the system with:

    sudo apt-get install module-assistant
    sudo apt-get install raspberrypi-kernel-headers
    sudo m-a prepare

thereafter, for each module,

    cd <module directory>
    make
    insmod <module name>.ko

See the README file in each module directory for operating instructions.

Send any feedback or suggestions to carla@fi.infn.it, they are welcome!

