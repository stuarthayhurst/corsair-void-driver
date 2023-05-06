// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for Corsair Void headsets

 * Copyright (c) 2023 Stuart Hayhurst
*/

/* ------------------------------------------------------------------------- */
/* Receiver report information: (ID 100)                                     */
/* ------------------------------------------------------------------------- */
/*
 - When queried, the receiver seems to respond with a lot of data for little information, as HID event
 - The following information has been observed for report ID 100

PROPERTY: USAGE CODE RANGE
  (?): -3866520 -> -3866513
    - Seems to always be 0

  BATTERY CAPACITY: -3866512
    - Seems to report 54 higher than reality when charging
    - Seems to be capped at 100

  CONNECTION STATUS: -3866511
    - 0: Disconnected
    - 1: Connected
    - 3: Searching
    - 6: Initialising

  (?): -3866509 -> -3866510
    - Seems to always be 1

  (?): -3866508
    - Seems to always be 0

  BATTERY STATUS: -3866507
    - 0     : Disconnected
    - 1 / 2 : Normal / low
    - 3     : Unknown (not seen)
    - 4 / 5 : Charging
*/
/* ------------------------------------------------------------------------- */

#define DRIVER_NAME "corsair-void"

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/power_supply.h>
#include <linux/completion.h>

#include <linux/version.h>

#include "hid-ids.h"

#define CORSAIR_VOID_BATT_CAPACITY_USAGE -3866512
#define CORSAIR_VOID_CONNECTION_USAGE -3866511
#define CORSAIR_VOID_BATT_STATUS_USAGE -3866507

#define CORSAIR_VOID_CONTROL_REQUEST 0x09
#define CORSAIR_VOID_CONTROL_REQUEST_TYPE 0x21
#define CORSAIR_VOID_CONTROL_INDEX 3

#define CORSAIR_VOID_CONTROL_BATT_VALUE 0x02C9
#define CORSAIR_VOID_CONTROL_ALERT_VALUE 0x02CA

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

#define CORSAIR_VOID_CAPACITY_BIT 0x01
#define CORSAIR_VOID_CONNECTION_BIT 0x02
#define CORSAIR_VOID_STATUS_BIT 0x04
#define CORSAIR_VOID_ALL_BITS 0x07

struct corsair_void_raw_receiver_info {
	bool waiting;
	struct completion query_completed;
	unsigned char received_bits;

	int battery_capacity;
	int connection_status;
	int battery_status;
};

struct corsair_void_drvdata {
	struct hid_device *hid_dev;
	struct device *dev;

	char* name;

	struct corsair_void_raw_receiver_info raw_receiver_info;
	struct corsair_void_battery_data battery_data;

	struct power_supply *batt;
	struct power_supply_desc batt_desc;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
static void corsair_void_set_wireless_status(struct corsair_void_drvdata *drvdata)
{
	struct usb_interface *usb_if = to_usb_interface(drvdata->dev->parent);

	usb_set_wireless_status(usb_if, drvdata->batt_data->present ?
					USB_WIRELESS_STATUS_CONNECTED :
					USB_WIRELESS_STATUS_DISCONNECTED);
}
#endif

static void corsair_void_set_unknown_data(struct corsair_void_drvdata *drvdata)
{
	struct corsair_void_battery_data *batt_data = &drvdata->battery_data;

	batt_data->status = POWER_SUPPLY_STATUS_UNKNOWN;
	batt_data->present = 0;
	batt_data->capacity = 0;
	batt_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
	corsair_void_set_wireless_status(drvdata);
#endif
}

static void corsair_void_process_receiver(struct corsair_void_drvdata *drvdata) {
	struct corsair_void_raw_receiver_info *raw_receiver_info = &drvdata->raw_receiver_info;
	struct corsair_void_battery_data *batt_data = &drvdata->battery_data;

	//Check connection and battery status to set battery data
	if (raw_receiver_info->connection_status != 1) {
		//Headset not connected
		goto unknown_data;
	} else if (raw_receiver_info->battery_status == 0) {
		//Battery information unavailable
		goto unknown_data;
	} else {
		//Battery connected
		batt_data->present = 1;
		batt_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

		//Set battery status
		switch (raw_receiver_info->battery_status) {
		case 1:
		case 2: //Battery normal / low
			batt_data->status = POWER_SUPPLY_STATUS_DISCHARGING;
			if (raw_receiver_info->battery_status == 2) {
				batt_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
			}

			break;
		case 4:
		case 5: //Battery charging
			batt_data->status = POWER_SUPPLY_STATUS_CHARGING;
			break;
		default:
			goto unknown_data;
			break;
		}

		batt_data->capacity = raw_receiver_info->battery_capacity;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
	corsair_void_set_wireless_status(drvdata);
#endif

	goto success;
unknown_data:
	corsair_void_set_unknown_data(drvdata);
success:
	return;
}

static int corsair_void_query_receiver(struct corsair_void_drvdata *drvdata)
{
	int ret = 0;

	struct usb_interface *usb_if = to_usb_interface(drvdata->dev->parent);
	struct usb_device *usb_dev = interface_to_usbdev(usb_if);

	struct corsair_void_raw_receiver_info *raw_receiver_info = &drvdata->raw_receiver_info;

	unsigned char send_buf[2] = {0xC9, 0x64};
	unsigned long expire = 0;

	//Prepare a completion to wait for return data
	if (!raw_receiver_info->waiting) {
	  init_completion(&raw_receiver_info->query_completed);
	  raw_receiver_info->received_bits = 0;
	}

	ret = usb_control_msg_send(usb_dev, 0,
			CORSAIR_VOID_CONTROL_REQUEST, CORSAIR_VOID_CONTROL_REQUEST_TYPE,
			CORSAIR_VOID_CONTROL_BATT_VALUE, CORSAIR_VOID_CONTROL_INDEX,
			send_buf, 2,
			USB_CTRL_SET_TIMEOUT, GFP_KERNEL);

	if (ret) {
		printk(KERN_WARNING DRIVER_NAME": failed to query receiver data (reason %i)", ret);
		goto unknown_data;
	}

	/*
	  - Wait 500ms for all receiver data to arrive
	  - Data is reported as a hid event, so we wait for the timeout, or all data to arrive
	  - In reality, it takes much less time than this
	*/
	expire = msecs_to_jiffies(500);
	raw_receiver_info->waiting = true;
	if (!wait_for_completion_timeout(&raw_receiver_info->query_completed, expire)) {
		ret = -ETIMEDOUT;
		printk(KERN_WARNING DRIVER_NAME": failed to query receiver data (reason %i)", ret);
		raw_receiver_info->waiting = false;
		goto unknown_data;
	}
	raw_receiver_info->waiting = false;

goto success;
unknown_data:
	corsair_void_set_unknown_data(drvdata);
success:
	return ret;
}

static int corsair_void_battery_get_property(struct power_supply *psy,
					     enum power_supply_property prop,
					     union power_supply_propval *val)
{
	struct corsair_void_drvdata *drvdata = power_supply_get_drvdata(psy);
	int ret = 0;

	//Handle any properties that don't require a query
	switch (prop) {
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
			goto query_required;
			break;
	}

	return ret;

query_required:
	ret = corsair_void_query_receiver(drvdata);

	if (ret) {
		goto query_failed;
	}

	switch (prop) {
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
		default:
			ret = -EINVAL;
			break;
	}

query_failed:
	return ret;
}

static ssize_t corsair_void_send_alert(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int ret = 0;

	struct usb_interface *usb_if = to_usb_interface(dev->parent);
	struct usb_device *usb_dev = interface_to_usbdev(usb_if);

	unsigned char* send_buf;
	int alert_id;

	ret = kstrtoint(buf, 10, &alert_id);
	if (ret) {
		return -EINVAL;
	}

	//Only accept 0 or 1 for alert ID
	if (alert_id != 0 && alert_id != 1) {
		return -EINVAL;
	}

	send_buf = kmalloc(3, GFP_KERNEL);
	if (!send_buf) {
		ret = -ENOMEM;
		goto failed_alloc;
	}

	//Packet format to send alert with ID alert_id
	send_buf[0] = 0xCA;
	send_buf[1] = 0x02;
	send_buf[2] = alert_id;

	ret = usb_control_msg_send(usb_dev, 0,
			CORSAIR_VOID_CONTROL_REQUEST, CORSAIR_VOID_CONTROL_REQUEST_TYPE,
			CORSAIR_VOID_CONTROL_ALERT_VALUE, CORSAIR_VOID_CONTROL_INDEX,
			send_buf, 3,
			USB_CTRL_SET_TIMEOUT, GFP_KERNEL);

	if (ret) {
		dev_warn(dev, "Failed to send alert request (reason %i)", ret);
		goto failed;
	}

	ret = count;
failed:
	kfree(send_buf);
failed_alloc:
	return ret;
}

//Write-only alert, as it only plays a sound (nothing to report back)
static DEVICE_ATTR(send_alert, 0200, NULL, corsair_void_send_alert);

static struct attribute *corsair_void_attrs[] = {
	&dev_attr_send_alert.attr,
	NULL,
};

static const struct attribute_group corsair_void_attr_group = {
	.attrs = corsair_void_attrs,
};

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
	if (ret) {
		hid_err(hid_dev, "parse failed\n");
		return ret;
	}
	ret = hid_hw_start(hid_dev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hid_dev, "hw start failed\n");
		return ret;
	}

	drvdata->dev = dev;
	drvdata->hid_dev = hid_dev;

	name = devm_kzalloc(dev, 14, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto failed_after_hid_start;
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
		goto failed_after_hid_start;
	}

	corsair_void_set_unknown_data(drvdata);

	ret = power_supply_powers(drvdata->batt, dev);
	if (ret) {
		goto failed_after_hid_start;
	}

	ret = sysfs_create_group(&dev->kobj, &corsair_void_attr_group);
	if (ret) {
		goto failed_after_hid_start;
	}

	//Any failures after here should go to failed_after_sysfs

	goto success;

//failed_after_sysfs:
//	sysfs_remove_group(&hid_dev->dev.kobj, &corsair_void_attr_group);
failed_after_hid_start:
	hid_hw_stop(hid_dev);
success:
	return ret;
}

static void corsair_void_remove(struct hid_device *hid_dev)
{
	sysfs_remove_group(&hid_dev->dev.kobj, &corsair_void_attr_group);
	hid_hw_stop(hid_dev);
}

static int corsair_void_event(struct hid_device *hid_dev, struct hid_field *field,
			      struct hid_usage *usage, __s32 value)
{
	struct corsair_void_drvdata *drvdata = hid_get_drvdata(hid_dev);

	switch (usage->hid) {
	case CORSAIR_VOID_BATT_CAPACITY_USAGE:
		drvdata->raw_receiver_info.battery_capacity = value;
		drvdata->raw_receiver_info.received_bits |= CORSAIR_VOID_CAPACITY_BIT;
		break;
	case CORSAIR_VOID_CONNECTION_USAGE:
		drvdata->raw_receiver_info.connection_status = value;
		drvdata->raw_receiver_info.received_bits |= CORSAIR_VOID_CONNECTION_BIT;
		break;
	case CORSAIR_VOID_BATT_STATUS_USAGE:
		drvdata->raw_receiver_info.battery_status = value;
		drvdata->raw_receiver_info.received_bits |= CORSAIR_VOID_STATUS_BIT;
		break;
	default:
		break;
	}

	//When all expected attributes have been detected, finish
	if (drvdata->raw_receiver_info.received_bits == CORSAIR_VOID_ALL_BITS) {
		corsair_void_process_receiver(drvdata);
		complete(&drvdata->raw_receiver_info.query_completed);
	}

	return 0;
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
	.name = DRIVER_NAME,
	.id_table = corsair_void_devices,
	.probe = corsair_void_probe,
	.remove = corsair_void_remove,
	.event = corsair_void_event,
};

module_hid_driver(corsair_void_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stuart Hayhurst");
MODULE_DESCRIPTION("HID driver for Corsair Void headsets");

/* TODO:
 - Document driver attributes (list from README)
 - See if the battery + alert packets can be done via hid
 - Check which calls are actually needed to read data (parse?)
 - Test wireless_status on kernel 6.4
*/
