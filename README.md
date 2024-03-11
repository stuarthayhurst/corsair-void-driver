## corsair-void-driver
  - Corsair Void headset family device driver for the Linux kernel
    - Includes the Corsair Void Pro / Elite / RGB headsets
    - This is experimental, expect issues
  - Requires kernel headers to be installed
  - I'll eventually upstream this driver, but I'd rather get it feature-complete and stable first

## Build system
  - `make`: Build the module
  - `make install`: Install the module
  - `make clean`: Clean the build directory
  - A different build directory can be forced with `BUILD_DIR=[DIR] make ...`
    - Defaults to `build`
  - Kernel headers are responsible for the install, look for `/usr/lib/modules/[KERNEL VERSION]/extra/corsair-void.ko` to remove it
    - On some systems, this might be `/usr/lib/modules/[KERNEL VERSION]/updates/corsair-void.ko`

## Features
  - [x] Battery reporting
  - [ ] LED support (on / off, brightness, colour)
  - [x] Sidetone support `(sysfs) set_sidetone [0 - 55] (write-only)`
  - [x] Misc device / kernel attributes
    - [x] `(USB) wireless_status` (requires kernel 6.4+)
    - [x] `(sysfs) microphone_up: [0 / 1] (read-only)`
    - [x] `(sysfs) send_alert: [0 / 1] (write-only)`
    - [x] `(sysfs) fw_version_[receiver / headset] (read-only)`
  - [ ] Wired, wireless and surround headset support
    - Currently, only wireless headsets are properly supported
    - If you have a spare wired or surround variant, please get in touch
      - If it's not spare, feel free to hack away at this driver

## References:
  - [headsetcontrol](https://github.com/Sapd/HeadsetControl/blob/master/src/devices/corsair_void.c)
  - [hid-corsair](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/hid/hid-corsair.c)
  - [hid-logitech](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/hid/hid-logitech-hidpp.c)
