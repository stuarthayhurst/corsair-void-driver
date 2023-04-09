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

#define CORSAIR_VOID_CONTROL_REQUEST 0x09
#define CORSAIR_VOID_CONTROL_REQUEST_TYPE 0x21
#define CORSAIR_VOID_CONTROL_VALUE 0x02c9
#define CORSAIR_VOID_CONTROL_INDEX 3
#define CORSAIR_VOID_ENDPOINT_IN 0x83

#define CORSAIR_VOID_BATT_DATA_SIZE 5
#define CORSAIR_VOID_MIC_UP 128

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

/* TODO Working hidapi calls:
  ret = hid_write(hid_dev, [0xC9, 0x64], 2);
  ret = hid_read_timeout(hid_dev, read_data, 5, timeout);
*/

/* TODO: this describes the endpoint data is sent back from the headset on
Endpoint Descriptor:
  bLength                 7
  bDescriptorType         5
  bEndpointAddress     0x83  EP 3 IN
  bmAttributes            3
    Transfer Type            Interrupt
    Synch Type               None
    Usage Type               Data
  wMaxPacketSize     0x0020  1x 32 bytes
  bInterval               1
*/

/* data_buf:
Index : 0       , 1, 2         , 3      , 4
Info  : ID (100), 0, CHARGE (%), STATUS?, BATTERY STATUS
*/

//Capacity seems to report 54 higher when charging?

//status:
//38, 51, 177

/*BATTERY STATUS:
0: disconnected
1/2: normal / low
3: unknown (not seen)
4/5: charging
*/

//51: searching?
//177: connected?
//38: ?? gave up? - gave after a long beep - error?

static int corsair_void_read_battery(struct corsair_void_drvdata *drvdata)
{
	int ret = 0;

	struct device *dev = drvdata->dev;
	struct corsair_void_battery_data *batt_data = &drvdata->battery_data;

	struct usb_interface *usb_if = to_usb_interface(dev->parent);
	struct usb_device *usb_dev = interface_to_usbdev(usb_if);

	unsigned char send_buf[2] = {0xC9, 0x64};
	unsigned char *data_buf; //Allocated later, required for USB calls
	int actual_size = 0;

/* TODO: better approach
 - Send bulk message - test interrupt
 - Send control packet
 - Wait for return bulk packet
 - Process data
 - Effectively, just translate the hidapi calls to kernel space?
   - need a queue for this one, look into hid kernel api

 - Maybe look into the event handler more? send packet here, timeout if it hasn't had an event by then

 - Replace hex with macros
*/

//TODO: look into hid-core.c, usbhit_get/set_raw_report is promising
//hid_irq_in
//hit_submit_out
//Find the public versions

//TODO: ideal
/*ret = hid_hw_raw_request(drvdata->hid_dev, 100, data_buf, SIZE, HID_INPUT_REPORT,
				HID_REQ_GET_REPORT);*/

//TODO reduce retires
int retries = 5;


int i; //TODO debug
printk("starting");

	data_buf = kzalloc(CORSAIR_VOID_BATT_DATA_SIZE, GFP_KERNEL);
	if (!data_buf) {
		ret = -ENOMEM;
		goto failed_alloc;
	}

	do {

//TODO is this necessary?
		ret = usb_control_msg_send(usb_dev, 0,
				CORSAIR_VOID_CONTROL_REQUEST, CORSAIR_VOID_CONTROL_REQUEST_TYPE,
				CORSAIR_VOID_CONTROL_VALUE, CORSAIR_VOID_CONTROL_INDEX,
				send_buf, 2,
				USB_CTRL_SET_TIMEOUT, GFP_KERNEL);

		if (ret) {
			goto ctrl_failed;
		}

//TODO: send poll first, wrap ctrl
//TODO: when this is more reliable, use USB_CTRL_GET_TIMEOUT for the timeout, instead of 1000
	ret = usb_bulk_msg(usb_dev,
			usb_rcvbulkpipe(usb_dev, CORSAIR_VOID_ENDPOINT_IN),
			data_buf,
			CORSAIR_VOID_BATT_DATA_SIZE,
			&actual_size, 1000);

ctrl_failed:

//TODO debug
printk(KERN_INFO "  last ret: %i", ret);
printk(KERN_INFO "  code: %i", data_buf[0]);

	//Retry if it got the wrong packet, timed out or was interrupted, and has retries left
	} while (((ret == -EAGAIN || ret == -ETIMEDOUT) || (data_buf[0] != 100)) && --retries);

	//Failed to read battery data
	if (ret || data_buf[0] != 100 || actual_size != 5) {
		printk(KERN_WARNING "Failed to read battery data for %s", drvdata->batt_desc.name);
		goto unknown_data;
	}

	if (!ret && data_buf[4] != 0) {
		batt_data->status = POWER_SUPPLY_STATUS_UNKNOWN;
		batt_data->present = 1;
		if (data_buf[4] == 0) { //Headset disconnected
			batt_data->status = POWER_SUPPLY_STATUS_UNKNOWN;
			batt_data->present = 0;
		} else if (data_buf[4] == 1 || data_buf[4] == 2) { //Battery normal / low
			batt_data->status = POWER_SUPPLY_STATUS_DISCHARGING;
		} else if (data_buf[4] == 4 || data_buf[4] == 5) { //Battery charging
			batt_data->status = POWER_SUPPLY_STATUS_CHARGING;
		} else {
			goto unknown_data;
		}

		//Ignore mic status
		if (data_buf[2] & CORSAIR_VOID_MIC_UP) {
			data_buf[2] = data_buf[2] & ~CORSAIR_VOID_MIC_UP;
		}
		batt_data->capacity = data_buf[2];

//TODO: Set capacity level from a case
batt_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;


//TODO debug
for (i = 0; i < CORSAIR_VOID_BATT_DATA_SIZE; i++) {
	printk(KERN_INFO "DATA %i : %i", i, data_buf[i]);
}

	} else {
		goto unknown_data;
	}

goto success;
unknown_data:
	kfree(data_buf);
failed_alloc:
	batt_data->status = POWER_SUPPLY_STATUS_UNKNOWN;
	batt_data->present = 0;
	batt_data->capacity = 0;
	batt_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	return ret;

success:
	kfree(data_buf);
	return ret;
}

static int corsair_void_battery_get_property(struct power_supply *psy,
					     enum power_supply_property psp,
					     union power_supply_propval *val)
{
	struct corsair_void_drvdata *drvdata = power_supply_get_drvdata(psy);
	int ret = 0;

	ret = corsair_void_read_battery(drvdata);

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
	if (!drvdata) {
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
	if (!name) {
		ret = -ENOMEM;
		goto failed;
	}
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
 - Set device class for upower (might need linking devices? fix report descriptor?)
 - Better approach to battery setting, read more data, properly
 - Check which calls are actually needed to read data (parse?)
 - Check which headers are actually required
 - Clean up code
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
