kRad
====

Linux kernel module implementing a hardware random number generator for geiger counters.

Currently setup to listen to the Raspberry Pi's GPIO on pin 11 (physical). Attach your geiger counter's `pulse` signal to this pin.

**warning** this module has *not* been tested for [FIPS 140-2](https://en.wikipedia.org/wiki/FIPS_140-2) compliance yet. Use at your own risk.
