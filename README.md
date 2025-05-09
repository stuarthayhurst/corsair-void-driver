## corsair-void-driver
  - Corsair Void headset family device driver for the Linux kernel
    - Includes the Corsair Void Pro / Elite / RGB headsets
  - Upstreamed for kernel 6.13+
  - Requires kernel headers to be installed
  - Kernels before 6.10 are only supported up to tag `v0.2`
  - Kernels before 6.4 are only supported up to tag `v0.1`

## Upstream
  - This driver has been upstreamed, but this repository is still useful for users with pre-6.13 kernels
  - Developers hacking on this driver can rebuild the module from here, rather than a whole kernel
  - If you're running kernel 6.13+ and the driver isn't available, check `CONFIG_HID_CORSAIR` is set to `y` or `m`

## Build system
  - `make`: Build the module
  - `make install`: Install the module
  - `make clean`: Clean the build directory
  - A different build directory can be forced with `BUILD_DIR=[DIR] make ...`
    - Defaults to `build`
  - Kernel headers are responsible for the install, look for `/usr/lib/modules/[KERNEL VERSION]/extra/hid-corsair-void.ko` to remove it
    - On some systems, this might be `/usr/lib/modules/[KERNEL VERSION]/updates/hid-corsair-void.ko`

## Features
  - [x] Battery reporting
  - [ ] LED support (on / off, brightness, colour)
    - I currently have no set plans to tackle this, but pull requests are welcome
    - For anyone attempting this, here's a rough check-list:
      - [ ] Software control mode on probe
      - [ ] Handlers for both mutes
      - [ ] Audio alerts when mute state changes
        - [ ] Audio alerts for wired devices
        - [ ] Share the code for this with sysfs
      - [ ] LED control functions
      - [ ] Sysfs LED support
  - [x] Sidetone support
    - [x] `(sysfs) set_sidetone: [0 - sidetone_max] (write-only)`
    - [x] `(sysfs) sidetone_max (read-only)`
  - [x] Misc device / kernel attributes
    - [x] `(USB) wireless_status (wireless only)`
    - [x] `(sysfs) microphone_up: [0 / 1] (read-only)`
    - [x] `(sysfs) send_alert: [0 / 1] (write-only), (wireless only)`
      - If this can be done on wired headsets, feel free to submit a pull request
    - [x] `(sysfs) fw_version_[receiver / headset] (read-only)`
  - [x] Wired, wireless and surround headset support
    - Wired and surround headsets aren't as well tested
      - If you have one of these, please file an issue with whether or not the sidetone works

## References:
  - [headsetcontrol](https://github.com/Sapd/HeadsetControl/blob/master/src/devices/corsair_void.c)
  - [hid-corsair](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/hid/hid-corsair.c)
  - [hid-logitech](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/hid/hid-logitech-hidpp.c)
