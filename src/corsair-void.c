// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for Corsair Void headsets

 * Copyright (c) 2023 Stuart Hayhurst
*/

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/power_supply.h>

#include "hid-ids.h"

//TODO: add more
static enum power_supply_property corsair_void_battery_props[] = {
//POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
//POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

struct corsair_void_drvdata {
	struct hid_device *hid_dev;
	struct device *dev;

	int capacity;

	struct power_supply *batt;
	struct power_supply_desc batt_desc;
};

static int corsair_void_battery_get_property(struct power_supply *psy,
					     enum power_supply_property psp,
					     union power_supply_propval *val)
{
	struct corsair_void_drvdata *drvdata = power_supply_get_drvdata(psy);
	int ret = 0;

//TODO: call function to populate battery info

//TODO Add more attributes

	switch (psp) {
//TODO: Replace comments with properties
//		case POWER_SUPPLY_PROP_STATUS:
		case POWER_SUPPLY_PROP_CAPACITY:
			//TODO: read saved data
			val->intval = 100;
			break;
//		case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		case POWER_SUPPLY_PROP_SCOPE:
			val->intval = POWER_SUPPLY_SCOPE_DEVICE;
			break;
		case POWER_SUPPLY_PROP_MODEL_NAME:
			char *name = drvdata->hid_dev->name;
			if (!strncmp(name, "Corsair ", 8))
				val->strval = name + 8;
			else
				val->strval = name;
			break;
		case POWER_SUPPLY_PROP_MANUFACTURER:
			val->strval = "Corsair";
			break;
//		case POWER_SUPPLY_PROP_SERIAL_NUMBER:
//			val->strval = drvdata->hid_dev->uniq;
		default:
			ret = -EINVAL;
			break;
	}

	return ret;
}

static int corsair_void_probe(struct hid_device *hid_dev, const struct hid_device_id *hid_id)
{
	int ret = 0;
	char name[32];
	struct corsair_void_drvdata *drvdata;
	struct power_supply_config psy_cfg;
	struct device *dev;

	dev = &hid_dev->dev;

	if (!hid_is_usb(hid_dev)) {
		ret = -EINVAL;
		return ret;
	}

	drvdata = devm_kzalloc(dev, sizeof(struct corsair_void_drvdata),
			       GFP_KERNEL);

	if (drvdata == NULL) {
		ret = -ENOMEM;
		return ret;
	}
	hid_set_drvdata(hid_dev, drvdata);
	psy_cfg.drv_data = drvdata;

	ret = hid_parse(hid_dev);
	if (ret != 0) {
		hid_err(hid_dev, "parse failed\n");
		return ret;
	}
	ret = hid_hw_start(hid_dev, HID_CONNECT_DEFAULT);
	if (ret != 0) {
		hid_err(hid_dev, "hw start failed\n");
		return ret;
	}

	drvdata->dev = dev;
	drvdata->hid_dev = hid_dev;

//TODO: replace (maybe?) - use a struct to store all battery data
	drvdata->capacity = 100;

        snprintf(name, sizeof(name), "hid-%d-battery", hid_dev->id);
	drvdata->batt_desc.name = name;

	drvdata->batt_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drvdata->batt_desc.properties = corsair_void_battery_props;
	drvdata->batt_desc.num_properties = ARRAY_SIZE(corsair_void_battery_props);
	drvdata->batt_desc.get_property = corsair_void_battery_get_property;

	drvdata->batt = devm_power_supply_register(dev, &drvdata->batt_desc, &psy_cfg);
	if (IS_ERR(drvdata->batt)) {
		dev_err(drvdata->dev, "failed to register battery\n");
		ret = PTR_ERR(drvdata->batt);
		goto failed;
	}

	power_supply_powers(drvdata->batt, dev);

	goto success;

failed:
	hid_hw_stop(hid_dev);
success:
	return ret;
}

static void corsair_void_remove(struct hid_device *hid_dev)
{
//TODO
	//struct corsair_void_drvdata *drvdata = hid_get_drvdata(hid_dev);
	//power_supply_unregister(drvdata->batt);

	hid_hw_stop(hid_dev);
}

static const struct hid_device_id corsair_void_devices[] = {
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_WIRELESS)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_PRO)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_PRO_R2)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_PRO_USB)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_PRO_USB_2)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_PRO_WIRELESS)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_USB)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_USB_2)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_USB_2)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_WIRED)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_WIRELESS)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_ELITE_WIRELESS)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_ELITE_WIRELESS_PREMIUM)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_ELITE_USB)},
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_ELITE_WIRELESS)},
  {}
};

MODULE_DEVICE_TABLE(hid, corsair_void_devices);

static struct hid_driver corsair_void_driver = {
	.name = "corsair-void",
	.id_table = corsair_void_devices,
	.probe = corsair_void_probe,
	.remove = corsair_void_remove,
};

module_hid_driver(corsair_void_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stuart Hayhurst");
MODULE_DESCRIPTION("HID driver for Corsair Void headsets");

/*TODO:
 - Fix segfault on corsair_remove (cancel work somehow?)
 - Boilerplate prep to actually read attributes
 - Set device class for upower (might need linking devices?)
 - Investigate disconnect handler
 - Actually read attributes (might need access to usbif, hid device prep / removal)
 - Add volume rocker support
 - Clean up code quality
*/

/* Planned attributes: (ask Corsair for datasheet)
 - firmware revision
 - hardware revision
 - status
 - capacity level
 - Check Logitech driver + Corsair windows driver + headsetcontrol to build complete list
*/

/* Plans:
 - Create full list of planned features (LEDs, notification, sidetone, mic status)
 - Create full list of device and battery attributes planned
*/

/* Device boilerplate fixes:
 - Look into device matching (avoids large table)
*/

/* Code style fixes:
 - Check against kernel programming style
 - Check tabs / spaces
*/
