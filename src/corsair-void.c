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
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

struct corsair_void_battery_data {
	int status;
	int present;
	int capacity;
	int capacity_level;
};

struct corsair_void_drvdata {
	struct hid_device *hid_dev;
	struct device *dev;

	char* name;

	struct corsair_void_battery_data battery_data;

	struct power_supply *batt;
	struct power_supply_desc batt_desc;
};

static int corsair_void_read_battery(struct corsair_void_drvdata *drvdata)
{
	int ret = 0;

	struct hid_device *hid_dev = drvdata->hid_dev;
	struct corsair_void_battery_data *batt_data = &drvdata->battery_data;

/* TODO:
 - Read actual data from the device
 - Add return codes
*/

	batt_data->status = POWER_SUPPLY_STATUS_UNKNOWN;
	batt_data->present = 1;
	batt_data->capacity = 100;
	batt_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

	return ret;
}

static int corsair_void_battery_get_property(struct power_supply *psy,
					     enum power_supply_property psp,
					     union power_supply_propval *val)
{
	struct corsair_void_drvdata *drvdata = power_supply_get_drvdata(psy);
	int ret = 0;

	corsair_void_read_battery(drvdata);

	switch (psp) {
		case POWER_SUPPLY_PROP_STATUS:
			val->intval = drvdata->battery_data.status;
			break;
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = drvdata->battery_data.present;
			break;
		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = drvdata->battery_data.capacity;
			break;
		case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
			val->intval = drvdata->battery_data.capacity_level;
			break;
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
		default:
			ret = -EINVAL;
			break;
	}

	return ret;
}

static int corsair_void_probe(struct hid_device *hid_dev, const struct hid_device_id *hid_id)
{
	int ret = 0;
	struct corsair_void_drvdata *drvdata;
	struct power_supply_config psy_cfg;
	struct device *dev;
	char *name;

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

	name = devm_kzalloc(dev, 14, GFP_KERNEL);
	sprintf(name, "hid-%02d-battery", hid_dev->id);

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
//TODO: done by devm, doesn't work (segfault on device disconnect / module unload)
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
 - Fix segfault on corsair_remove (cancel work somehow? take out a lock? blood sacrifice?)
 - Set device class for upower (might need linking devices?)
 - Investigate disconnect handler
 - Actually read attributes (might need access to usbif, hid device prep / removal)
 - Check which calls are actually needed to read data (parse?)
 - Check which headers are actually required
 - Clean up code quality
*/

/* Planned attributes: (ask Corsair for datasheet)
 - firmware revision
 - hardware revision
 - Check Logitech driver + Corsair windows driver + headsetcontrol to build complete list
 - Check documentation for more battery properties
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
