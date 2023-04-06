## corsair-void-driver
  - Linux kernel driver for the Corsair Void family of headsets
    - This is experimental, expect issues
  - Requires kernel headers to be installed
  - I'll eventually upstream this driver, but I'd rather get it feature-complete and stable first

**WARNING: This currently causes a page fault most of the time when it's unloaded**

## Build system
  - `make`: Build the module
  - `make install`: Install the module
  - `make clean`: Clean the build directory
  - A different build directory can be forced with `BUILD_DIR=[DIR] make ...`
    - Defaults to `build`
  - Kernel headers are responsible for the install, look for `/usr/lib/modules/[KERNEL VERSION]/extra/corsair-void.ko` to remove it

## Features
  - [ ] Battery reporting
  - [ ] LED support
  - [ ] Sidetone support
  - [ ] Notification support
  - [ ] Misc sysfs attributes (firmware revision, hardware revision, etc)
