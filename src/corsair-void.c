// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for Corsair Void headsets
 * Report issues to https://github.com/stuarthayhurst/corsair-void-driver/issues

 * Copyright (c) 2023 Stuart Hayhurst
*/

/* ------------------------------------------------------------------------- */
/* Receiver report information: (ID 100)                                     */
/* ------------------------------------------------------------------------- */
/*
 - When queried, the receiver reponds with 5 bytes to describe the battery
  - The power button, mute button and moving the mic also trigger this report
 - This includes power button + mic + connection + battery status and capacity
 - The information below may not be perfect, it's been gathered through guesses

INDEX: PROPERTY
 0: REPORT ID
  - 100 for the battery packet

 1: POWER BUTTON + (?)
  - Largest bit is 1 when power button pressed

 2: BATTERY CAPACITY + MIC STATUS
  - Battery capacity:
   - Seems to report ~54 higher than reality when charging
   - Seems to be capped at 100
  - Microphone status:
   - Largest bit is set to 1 when the mic is physically up
   - No bits change when the mic is muted, only when physically moved
   - This report is sent every time the mic is moved, no polling required

 3: CONNECTION STATUS
  - 38: Initialising
  - 49: Lost connection
  - 51: Disconnected, searching
  - 52: Disconnected, not searching
  - 177: Normal

 4: BATTERY STATUS
  - 0: Disconnected
  - 1: Normal
  - 2: Low
  - 3: Critical - sent during shutdown
  - 4: Fully charged
  - 5: Charging
*/
/* ------------------------------------------------------------------------- */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/usb.h>

/* Only required for pre-Linux 6.4 support */
#include <linux/version.h>

#include "hid-ids.h"

#define CORSAIR_VOID_CONTROL_REQUEST 0x09
#define CORSAIR_VOID_CONTROL_REQUEST_TYPE 0x21
#define CORSAIR_VOID_CONTROL_INDEX 3

#define CORSAIR_VOID_CONTROL_ALERT_VALUE 0x02CA

#define CORSAIR_VOID_BATTERY_REPORT_ID 100

#define CORSAIR_VOID_MIC_MASK GENMASK(7, 7)
#define CORSAIR_VOID_CAPACITY_MASK GENMASK(6, 0)

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

struct corsair_void_raw_receiver_info {
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
	int mic_up;

	struct power_supply *battery;
	struct power_supply_desc battery_desc;
};

/*
 - Functions to process receiver data
*/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
static void corsair_void_set_wireless_status(struct corsair_void_drvdata *drvdata)
{
	struct usb_interface *usb_if = to_usb_interface(drvdata->dev->parent);

	usb_set_wireless_status(usb_if, drvdata->battery_data.present ?
					USB_WIRELESS_STATUS_CONNECTED :
					USB_WIRELESS_STATUS_DISCONNECTED);
}
#endif

static void corsair_void_set_unknown_data(struct corsair_void_drvdata *drvdata)
{
	struct corsair_void_battery_data *battery_data = &drvdata->battery_data;

	battery_data->status = POWER_SUPPLY_STATUS_UNKNOWN;
	battery_data->present = 0;
	battery_data->capacity = 0;
	battery_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

	drvdata->mic_up = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
	corsair_void_set_wireless_status(drvdata);
#endif
}

static void corsair_void_process_receiver(struct corsair_void_drvdata *drvdata) {
	struct corsair_void_raw_receiver_info *raw_receiver_info = &drvdata->raw_receiver_info;
	struct corsair_void_battery_data *battery_data = &drvdata->battery_data;
	struct corsair_void_battery_data orig_battery_data;
	int battery_struct_size = sizeof(struct corsair_void_battery_data);

	/* Save initial battery data, to compare later */
	orig_battery_data = *battery_data;

	/* Check connection and battery status to set battery data */
	if (raw_receiver_info->connection_status != 177) {
		/* Headset not connected */
		goto unknown_data;
	} else if (raw_receiver_info->battery_status == 0) {
		/* Battery information unavailable */
		goto unknown_data;
	} else {
		/* Battery connected */
		battery_data->present = 1;
		battery_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

		/* Set battery status */
		switch (raw_receiver_info->battery_status) {
		case 1:
		case 2:
		case 3: /* Battery normal / low / critical */
			battery_data->status = POWER_SUPPLY_STATUS_DISCHARGING;
			if (raw_receiver_info->battery_status == 2) {
				battery_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
			} else if (raw_receiver_info->battery_status == 3) {
				battery_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
			}

			break;
		case 4: /* Battery fully charged */
			battery_data->status = POWER_SUPPLY_STATUS_FULL;
			break;
		case 5: /* Battery charging */
			battery_data->status = POWER_SUPPLY_STATUS_CHARGING;
			break;
		default:
			dev_warn(drvdata->dev, "unknown receiver status '%i'",
				 raw_receiver_info->battery_status);
			goto unknown_data;
			break;
		}

		battery_data->capacity = raw_receiver_info->battery_capacity;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
	corsair_void_set_wireless_status(drvdata);
#endif

	goto success;
unknown_data:
	corsair_void_set_unknown_data(drvdata);
success:

	/* Decide if battery values changed */
	if (memcmp(&orig_battery_data, battery_data, battery_struct_size)) {
		power_supply_changed(drvdata->battery);
	}

	return;
}

/*
 - Functions to report stored data
*/

static int corsair_void_battery_get_property(struct power_supply *psy,
					     enum power_supply_property prop,
					     union power_supply_propval *val)
{
	struct corsair_void_drvdata *drvdata = power_supply_get_drvdata(psy);
	int ret = 0;

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

	return ret;
}

static ssize_t corsair_void_report_mic_up(struct device *dev,
					  struct device_attribute *attr,
					  char* buf)
{

	struct corsair_void_drvdata* drvdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%i\n", drvdata->mic_up);
}

/*
 - Functions to send data to headset
*/

static ssize_t corsair_void_send_alert(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int ret = 0;

	struct usb_interface *usb_if = to_usb_interface(dev->parent);
	struct usb_device *usb_dev = interface_to_usbdev(usb_if);

	unsigned char* send_buf;
	unsigned char alert_id;

	if (kstrtou8(buf, 10, &alert_id)) {
		return -EINVAL;
	}

	/* Only accept 0 or 1 for alert ID */
	if (alert_id >= 2) {
		return -EINVAL;
	}

	send_buf = kmalloc(3, GFP_KERNEL);
	if (!send_buf) {
		return -ENOMEM;
	}

	/* Packet format to send alert with ID alert_id */
	send_buf[0] = 0xCA;
	send_buf[1] = 0x02;
	send_buf[2] = alert_id;

	ret = usb_control_msg_send(usb_dev, 0,
			CORSAIR_VOID_CONTROL_REQUEST,
			CORSAIR_VOID_CONTROL_REQUEST_TYPE,
			CORSAIR_VOID_CONTROL_ALERT_VALUE,
			CORSAIR_VOID_CONTROL_INDEX,
			send_buf, 3,
			USB_CTRL_SET_TIMEOUT, GFP_KERNEL);

	if (ret) {
		dev_warn(dev, "failed to send alert request (reason %i)", ret);
	} else {
		ret = count;
	}

	kfree(send_buf);
	return ret;
}

/*
 - Driver setup, probing and HID event handling
*/

static DEVICE_ATTR(microphone_up, 0444, corsair_void_report_mic_up, NULL);
/* Write-only alert, as it only plays a sound (nothing to report back) */
static DEVICE_ATTR(send_alert, 0200, NULL, corsair_void_send_alert);

static struct attribute *corsair_void_attrs[] = {
	&dev_attr_microphone_up.attr,
	&dev_attr_send_alert.attr,
	NULL,
};

static const struct attribute_group corsair_void_attr_group = {
	.attrs = corsair_void_attrs,
};

static int corsair_void_probe(struct hid_device *hid_dev,
			      const struct hid_device_id *hid_id)
{
	int ret = 0;
	struct corsair_void_drvdata *drvdata;
	struct power_supply_config psy_cfg;
	char *name;
	int name_length;

	if (!hid_is_usb(hid_dev)) {
		return -EINVAL;
	}

	drvdata = devm_kzalloc(&hid_dev->dev, sizeof(struct corsair_void_drvdata),
			       GFP_KERNEL);
	if (!drvdata) {
		return -ENOMEM;
	}
	hid_set_drvdata(hid_dev, drvdata);
	psy_cfg.drv_data = drvdata;
	dev_set_drvdata(&hid_dev->dev, drvdata);

	drvdata->dev = &hid_dev->dev;
	drvdata->hid_dev = hid_dev;

	/* Set initial values for no headset attached */
	/* If a headset is attached, it'll send a packet soon enough */
	corsair_void_set_unknown_data(drvdata);

	ret = hid_parse(hid_dev);
	if (ret) {
		hid_err(hid_dev, "parse failed (reason: %i)\n", ret);
		return ret;
	}
	ret = hid_hw_start(hid_dev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hid_dev, "hid_hw_start failed (reason: %i)\n", ret);
		return ret;
	}

	name_length = snprintf(NULL, 0, "hid-%d-battery", hid_dev->id);
	name = devm_kzalloc(drvdata->dev, 14, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto failed_after_hid_start;
	}
	snprintf(name, name_length + 1, "hid-%d-battery", hid_dev->id);

	drvdata->battery_desc.name = name;
	drvdata->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drvdata->battery_desc.properties = corsair_void_battery_props;
	drvdata->battery_desc.num_properties = ARRAY_SIZE(corsair_void_battery_props);
	drvdata->battery_desc.get_property = corsair_void_battery_get_property;

	drvdata->battery = devm_power_supply_register(&hid_dev->dev,
						      &drvdata->battery_desc,
						      &psy_cfg);
	if (IS_ERR(drvdata->battery)) {
		ret = PTR_ERR(drvdata->battery);
		hid_err(hid_dev, "failed to register battery '%s' (reason: %i)\n",
			name, ret);
		goto failed_after_hid_start;
	}

	ret = power_supply_powers(drvdata->battery, drvdata->dev);
	if (ret) {
		goto failed_after_hid_start;
	}

	ret = sysfs_create_group(&hid_dev->dev.kobj, &corsair_void_attr_group);
	if (ret) {
		goto failed_after_hid_start;
	}

	/* Any failures after here should go to failed_after_sysfs */

	goto success;

/*failed_after_sysfs:
	sysfs_remove_group(&hid_dev->dev.kobj, &corsair_void_attr_group);*/
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

static int corsair_void_raw_event(struct hid_device *hid_dev,
				  struct hid_report *hid_report,
				  u8* data, int size)
{
	struct corsair_void_drvdata *drvdata = hid_get_drvdata(hid_dev);

	if (hid_report->id == CORSAIR_VOID_BATTERY_REPORT_ID) {
		/* Description of packet is documented at the top of this file */
		drvdata->raw_receiver_info.battery_capacity =
			FIELD_GET(CORSAIR_VOID_CAPACITY_MASK, data[2]);
		drvdata->raw_receiver_info.connection_status = data[3];
		drvdata->raw_receiver_info.battery_status = data[4];

		drvdata->mic_up = FIELD_GET(CORSAIR_VOID_MIC_MASK, data[2]);

		corsair_void_process_receiver(drvdata);
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
  {HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_VOID_RGB_USB_3)},
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
	.raw_event = corsair_void_raw_event,
};

module_hid_driver(corsair_void_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stuart Hayhurst");
MODULE_DESCRIPTION("HID driver for Corsair Void headsets");
