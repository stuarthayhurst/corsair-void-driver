// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for Corsair Void headsets

 * Copyright (c) 2023 Stuart Hayhurst
*/

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>

#include "corsair-void.h"

static const struct hid_device_id corsair_void_devices[] = {
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_PRO)},
  {}
};

MODULE_DEVICE_TABLE(hid, corsair_void_devices);

static struct hid_driver corsair_void_driver = {
	.name = "corsaid-void",
	.id_table = corsair_void_devices,
	/*.probe
	.event
	.remove*/
};

module_hid_driver(corsair_void_driver);

/* TODO:
 - Add more devices
 - Use correct header files for IDs
 - Device specific component over sysfs
 - README, .gitignore, repository
*/

/* Code fixes:
 - Kernel programming style
 - Check HID device is actually USB - check upstream commits to hid-corsair
 - Generic way to register devices?
 - Rename the driver
*/

/* TODO:
 - probe
 - event
 - remove

handle sysfs for battery and lights

*/

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stuart Hayhurst");
MODULE_DESCRIPTION("HID driver for Corsair Void headsets");
