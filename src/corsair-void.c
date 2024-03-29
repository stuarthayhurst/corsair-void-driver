// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for Corsair Void headsets
 * Report issues to https://github.com/stuarthayhurst/corsair-void-driver/issues

 * Copyright (c) 2023-2024 Stuart Hayhurst
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

/* ------------------------------------------------------------------------- */
/* Receiver report information: (ID 102)                                     */
/* ------------------------------------------------------------------------- */
/*
 - When queried, the recevier responds with 4 bytes to describe the firmware
 - The first 2 bytes are for the receiver, the second 2 are the headset
 - The headset firmware's version may be 0 if it's disconnected

INDEX: PROPERTY
 0: Recevier firmware major version
  - Major version of the receiver's firmware

 1: Recevier firmware minor version
  - Minor version of the receiver's firmware

 2: Headset firmware major version
  - Major version of the headset's firmware
  - This may be 0 if no headset is connected (version dependent)

 3: Headset firmware minor version
  - Minor version of the headset's firmware
  - This may be 0 if no headset is connected (version dependent)
*/
/* ------------------------------------------------------------------------- */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

/* Only required for pre-Linux 6.4 support */
#include <linux/version.h>

#include "hid-ids.h"

#define CORSAIR_VOID_STATUS_REQUEST_ID		0xC9
#define CORSAIR_VOID_NOTIF_REQUEST_ID		0xCA
#define CORSAIR_VOID_SIDETONE_REQUEST_ID	0xFF
#define CORSAIR_VOID_BATTERY_REPORT_ID		0x64
#define CORSAIR_VOID_FIRMWARE_REPORT_ID		0x66

#define CORSAIR_VOID_MIC_MASK			GENMASK(7, 7)
#define CORSAIR_VOID_CAPACITY_MASK		GENMASK(6, 0)

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

	char *name;

	struct corsair_void_battery_data battery_data;
	int mic_up;
	int connected;
	int fw_receiver_major;
	int fw_receiver_minor;
	int fw_headset_major;
	int fw_headset_minor;

	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	struct mutex battery_mutex;

	struct delayed_work delayed_status_work;
	struct delayed_work delayed_firmware_work;
	struct work_struct battery_remove_work;
	struct work_struct battery_add_work;
};

/*
 - Functions to process receiver data
*/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
static void corsair_void_set_wireless_status(struct corsair_void_drvdata *drvdata)
{
	struct usb_interface *usb_if = to_usb_interface(drvdata->dev->parent);

	usb_set_wireless_status(usb_if, drvdata->connected ?
					USB_WIRELESS_STATUS_CONNECTED :
					USB_WIRELESS_STATUS_DISCONNECTED);
}
#endif

static void corsair_void_set_unknown_batt(struct corsair_void_drvdata *drvdata)
{
	struct corsair_void_battery_data *battery_data = &drvdata->battery_data;

	battery_data->status = POWER_SUPPLY_STATUS_UNKNOWN;
	battery_data->present = 0;
	battery_data->capacity = 0;
	battery_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
}

static void corsair_void_set_unknown_data(struct corsair_void_drvdata *drvdata)
{
	/* Only 0 out headset firmware, receiver version is always be known */
	drvdata->fw_headset_major = 0;
	drvdata->fw_headset_minor = 0;

	drvdata->connected = 0;
	drvdata->mic_up = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
	corsair_void_set_wireless_status(drvdata);
#endif
}

static void corsair_void_process_receiver(struct corsair_void_drvdata *drvdata,
					  int raw_battery_capacity, int raw_connection_status,
					  int raw_battery_status)
{
	struct corsair_void_battery_data *battery_data = &drvdata->battery_data;
	struct corsair_void_battery_data orig_battery_data;
	int battery_struct_size = sizeof(struct corsair_void_battery_data);

	/* Save initial battery data, to compare later */
	orig_battery_data = *battery_data;

	/* Check connection and battery status to set battery data */
	if (raw_connection_status != 177) {
		/* Headset not connected */
		goto unknown_battery;
	} else if (raw_battery_status == 0) {
		/* Battery information unavailable */
		goto unknown_battery;
	} else {
		/* Battery connected */
		battery_data->present = 1;
		battery_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;

		/* Set battery status */
		switch (raw_battery_status) {
		case 1:
		case 2:
		case 3: /* Battery normal / low / critical */
			battery_data->status = POWER_SUPPLY_STATUS_DISCHARGING;
			if (raw_battery_status == 2) {
				battery_data->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
			} else if (raw_battery_status == 3) {
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
			hid_warn(drvdata->hid_dev, "unknown battery status '%d'",
				 raw_battery_status);
			goto unknown_battery;
			break;
		}

		battery_data->capacity = raw_battery_capacity;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
	corsair_void_set_wireless_status(drvdata);
#endif

	goto success;
unknown_battery:
	corsair_void_set_unknown_batt(drvdata);
success:

	/* Decide if battery values changed */
	if (memcmp(&orig_battery_data, battery_data, battery_struct_size)) {
		mutex_lock(&drvdata->battery_mutex);
		if (drvdata->battery) {
			power_supply_changed(drvdata->battery);
		}
		mutex_unlock(&drvdata->battery_mutex);
	}
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
			if (!strncmp(name, "Corsair ", 8)) {
				val->strval = name + 8;
			} else {
				val->strval = name;
			}
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
					  char *buf)
{
	struct corsair_void_drvdata *drvdata = dev_get_drvdata(dev);

	if (!drvdata->connected) {
		return -ENODEV;
	}

	return sysfs_emit(buf, "%d\n", drvdata->mic_up);
}

static ssize_t corsair_void_report_firmware(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct corsair_void_drvdata *drvdata = dev_get_drvdata(dev);
	int major, minor;

	if (!strcmp(attr->attr.name, "fw_version_receiver")) {
		major = drvdata->fw_receiver_major;
		minor = drvdata->fw_receiver_minor;
	} else {
		major = drvdata->fw_headset_major;
		minor = drvdata->fw_headset_minor;
	}

	if (major == 0 && minor == 0) {
		return -ENODATA;
	}

	return sysfs_emit(buf, "%d.%02d\n", major, minor);
}

/*
 - Functions to send data to headset
*/

static ssize_t corsair_void_send_alert(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct corsair_void_drvdata *drvdata = dev_get_drvdata(dev);
	struct hid_device *hid_dev = drvdata->hid_dev;
	unsigned char alert_id;
	unsigned char send_buf[3];
	int ret;

	if (!drvdata->connected) {
		return -ENODEV;
	}

	if (kstrtou8(buf, 10, &alert_id)) {
		return -EINVAL;
	}

	/* Only accept 0 or 1 for alert ID */
	if (alert_id >= 2) {
		return -EINVAL;
	}

	/* Packet format to send alert with ID alert_id */
	send_buf[0] = CORSAIR_VOID_NOTIF_REQUEST_ID;
	send_buf[1] = 0x02;
	send_buf[2] = alert_id;

	ret = hid_hw_raw_request(hid_dev, CORSAIR_VOID_NOTIF_REQUEST_ID,
			  send_buf, 3, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		hid_warn(hid_dev, "failed to send alert request (reason: %d)", ret);
		return ret;
	}

	return count;
}

static ssize_t corsair_void_send_sidetone(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct corsair_void_drvdata *drvdata = dev_get_drvdata(dev);
	struct hid_device *hid_dev = drvdata->hid_dev;
	unsigned char sidetone;
	unsigned char *send_buf;
	int ret;

	if (!drvdata->connected) {
		return -ENODEV;
	}

	if (kstrtou8(buf, 10, &sidetone)) {
		return -EINVAL;
	}

	/* sidetone must be between 0 and 55 inclusive */
	if (sidetone > 55) {
		return -EINVAL;
	}

	send_buf = kzalloc(64, GFP_KERNEL);
	if (!send_buf) {
		return -ENOMEM;
	}

	/* Packet format to set sidetone */
	send_buf[0] = CORSAIR_VOID_SIDETONE_REQUEST_ID;
	send_buf[1] = 0x0B;
	send_buf[2] = 0x00;
	send_buf[3] = 0xFF;
	send_buf[4] = 0x04;
	send_buf[5] = 0x0E;
	send_buf[6] = 0xFF;
	send_buf[7] = 0x05;
	send_buf[8] = 0x01;
	send_buf[9] = 0x04;
	send_buf[10] = 0x00;
	send_buf[11] = sidetone + 200;

	ret = hid_hw_raw_request(hid_dev, CORSAIR_VOID_SIDETONE_REQUEST_ID,
				 send_buf, 64, HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		hid_warn(hid_dev, "failed to send sidetone (reason: %d)", ret);
	} else {
		ret = count;
	}

	kfree(send_buf);
	return ret;
}

static int corsair_void_request_status(struct hid_device *hid_dev, int id)
{
	unsigned char send_buf[2];
	int ret;

	/* Packet format to request data item (battery / firmware) refresh */
	send_buf[0] = CORSAIR_VOID_STATUS_REQUEST_ID;
	send_buf[1] = id;

	/* Send request for data refresh */
	ret = hid_hw_raw_request(hid_dev, CORSAIR_VOID_STATUS_REQUEST_ID,
			  send_buf, 2, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		switch (id) {
		case CORSAIR_VOID_BATTERY_REPORT_ID:
			hid_warn(hid_dev, "failed to request battery (reason: %d)", ret);
			break;
		case CORSAIR_VOID_FIRMWARE_REPORT_ID:
			hid_warn(hid_dev, "failed to request firmware (reason: %d)", ret);
			break;
		default:
			hid_warn(hid_dev, "failed to send report %d (reason: %d)", id, ret);
			break;
		}
		return ret;
	}

	return 0;
}

/*
 - Headset connect / disconnect handlers and work handlers
*/

static void corsair_void_status_work_handler(struct work_struct *work)
{
	struct corsair_void_drvdata *drvdata;
	struct delayed_work *delayed_work;

	delayed_work = container_of(work, struct delayed_work, work);
	drvdata = container_of(delayed_work, struct corsair_void_drvdata, delayed_status_work);

	corsair_void_request_status(drvdata->hid_dev,
				    CORSAIR_VOID_BATTERY_REPORT_ID);
}

static void corsair_void_firmware_work_handler(struct work_struct *work)
{
	struct corsair_void_drvdata *drvdata;
	struct delayed_work *delayed_work;

	delayed_work = container_of(work, struct delayed_work, work);
	drvdata = container_of(delayed_work, struct corsair_void_drvdata, delayed_firmware_work);

	corsair_void_request_status(drvdata->hid_dev,
				    CORSAIR_VOID_FIRMWARE_REPORT_ID);
}

static void corsair_void_battery_remove_work_handler(struct work_struct *work)
{
	struct corsair_void_drvdata *drvdata;

	drvdata = container_of(work, struct corsair_void_drvdata, battery_remove_work);
	mutex_lock(&drvdata->battery_mutex);
	if (drvdata->battery) {
		power_supply_unregister(drvdata->battery);
		drvdata->battery = NULL;
	}
	mutex_unlock(&drvdata->battery_mutex);
}

static void corsair_void_battery_add_work_handler(struct work_struct *work)
{
	struct corsair_void_drvdata *drvdata;
	struct power_supply_config psy_cfg;

	drvdata = container_of(work, struct corsair_void_drvdata, battery_add_work);
	mutex_lock(&drvdata->battery_mutex);
	if (!drvdata->battery) {
		psy_cfg.drv_data = drvdata;
		drvdata->battery = power_supply_register(drvdata->dev,
							 &drvdata->battery_desc,
							 &psy_cfg);

		if (IS_ERR(drvdata->battery)) {
			hid_err(drvdata->hid_dev,
				"failed to register battery '%s' (reason: %ld)\n",
				drvdata->battery_desc.name,
				PTR_ERR(drvdata->battery));
			drvdata->battery = NULL;
			goto battery_unlock;
		}

		if (power_supply_powers(drvdata->battery, drvdata->dev)) {
			power_supply_unregister(drvdata->battery);
			drvdata->battery = NULL;
			goto battery_unlock;
		}
	}

battery_unlock:
	mutex_unlock(&drvdata->battery_mutex);
}

static void corsair_void_headset_connected(struct corsair_void_drvdata *drvdata)
{
	schedule_work(&drvdata->battery_add_work);
	schedule_delayed_work(&drvdata->delayed_firmware_work, msecs_to_jiffies(100));
}

static void corsair_void_headset_disconnected(struct corsair_void_drvdata *drvdata)
{
	schedule_work(&drvdata->battery_remove_work);

	corsair_void_set_unknown_data(drvdata);
	corsair_void_set_unknown_batt(drvdata);
}

/*
 - Driver setup, probing, HID event handling
*/

static DEVICE_ATTR(microphone_up, 0444, corsair_void_report_mic_up, NULL);
static DEVICE_ATTR(fw_version_receiver, 0444, corsair_void_report_firmware, NULL);
static DEVICE_ATTR(fw_version_headset, 0444, corsair_void_report_firmware, NULL);

/* Write-only alert, as it only plays a sound (nothing to report back) */
static DEVICE_ATTR(send_alert, 0200, NULL, corsair_void_send_alert);
/* Write-only alert, as sidetone volume can't be queried */
static DEVICE_ATTR(set_sidetone, 0200, NULL, corsair_void_send_sidetone);

static struct attribute *corsair_void_attrs[] = {
	&dev_attr_microphone_up.attr,
	&dev_attr_send_alert.attr,
	&dev_attr_set_sidetone.attr,
	&dev_attr_fw_version_receiver.attr,
	&dev_attr_fw_version_headset.attr,
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
	/* If a headset is attached, it'll be prompted later */
	corsair_void_set_unknown_data(drvdata);
	corsair_void_set_unknown_batt(drvdata);

	/* Set receiver firmware version, as set_unknown_data doesn't handle it */
	/* Receiver version won't be 0 after init during the driver's lifetime */
	drvdata->fw_receiver_major = 0;
	drvdata->fw_receiver_minor = 0;

	ret = hid_parse(hid_dev);
	if (ret) {
		hid_err(hid_dev, "parse failed (reason: %d)\n", ret);
		return ret;
	}

	name_length = snprintf(NULL, 0, "corsair-void-%d-battery", hid_dev->id);
	name = devm_kzalloc(drvdata->dev, name_length + 1, GFP_KERNEL);
	if (!name) {
		return -ENOMEM;
	}
	snprintf(name, name_length + 1, "corsair-void-%d-battery", hid_dev->id);

	drvdata->battery_desc.name = name;
	drvdata->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drvdata->battery_desc.properties = corsair_void_battery_props;
	drvdata->battery_desc.num_properties = ARRAY_SIZE(corsair_void_battery_props);
	drvdata->battery_desc.get_property = corsair_void_battery_get_property;

	drvdata->battery = NULL;
	INIT_WORK(&drvdata->battery_remove_work, corsair_void_battery_remove_work_handler);
	INIT_WORK(&drvdata->battery_add_work, corsair_void_battery_add_work_handler);
	mutex_init(&drvdata->battery_mutex);

	ret = sysfs_create_group(&hid_dev->dev.kobj, &corsair_void_attr_group);
	if (ret) {
		return ret;
	}

	ret = hid_hw_start(hid_dev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hid_dev, "hid_hw_start failed (reason: %d)\n", ret);
		goto failed_after_sysfs;
	}

	/* Any failures after here should go to failed_after_hid_start */

	/* Refresh battery data, in case headset is already connected */
	INIT_DELAYED_WORK(&drvdata->delayed_status_work,
			  corsair_void_status_work_handler);
	schedule_delayed_work(&drvdata->delayed_status_work, msecs_to_jiffies(100));

	/* Refresh firmware versions */
	INIT_DELAYED_WORK(&drvdata->delayed_firmware_work,
			  corsair_void_firmware_work_handler);
	schedule_delayed_work(&drvdata->delayed_firmware_work, msecs_to_jiffies(100));

	goto success;

/*failed_after_hid_start:
	hid_hw_stop(hid_dev);*/
failed_after_sysfs:
	sysfs_remove_group(&hid_dev->dev.kobj, &corsair_void_attr_group);
success:
	return ret;
}

static void corsair_void_remove(struct hid_device *hid_dev)
{
	struct corsair_void_drvdata *drvdata = hid_get_drvdata(hid_dev);

	hid_hw_stop(hid_dev);
	cancel_work_sync(&drvdata->battery_remove_work);
	cancel_work_sync(&drvdata->battery_add_work);
	if (drvdata->battery) {
		power_supply_unregister(drvdata->battery);
	}

	cancel_delayed_work_sync(&drvdata->delayed_firmware_work);
	sysfs_remove_group(&hid_dev->dev.kobj, &corsair_void_attr_group);
}

static int corsair_void_raw_event(struct hid_device *hid_dev,
				  struct hid_report *hid_report,
				  u8 *data, int size)
{
	struct corsair_void_drvdata *drvdata = hid_get_drvdata(hid_dev);
	int was_connected = drvdata->connected;

	/* Description of packets are documented at the top of this file */
	if (hid_report->id == CORSAIR_VOID_BATTERY_REPORT_ID) {
		drvdata->mic_up = FIELD_GET(CORSAIR_VOID_MIC_MASK, data[2]);
		drvdata->connected = !!(data[3] == 177);

		corsair_void_process_receiver(drvdata,
					      FIELD_GET(CORSAIR_VOID_CAPACITY_MASK, data[2]),
					      data[3], data[4]);
	} else if (hid_report->id == CORSAIR_VOID_FIRMWARE_REPORT_ID) {
		drvdata->fw_receiver_major = data[1];
		drvdata->fw_receiver_minor = data[2];
		drvdata->fw_headset_major = data[3];
		drvdata->fw_headset_minor = data[4];
	}

	/* Handle headset connect / disconnect */
	if (was_connected != drvdata->connected) {
		if (drvdata->connected) {
			corsair_void_headset_connected(drvdata);
		} else {
			corsair_void_headset_disconnected(drvdata);
		}
	}

	return 0;
}

static const struct hid_device_id corsair_void_devices[] = {
	/* Corsair Void Wireless */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a0c) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a2b) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x1b23) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x1b25) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x1b27) },

	/* Corsair Void USB */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a0f) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x1b1c) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x1b29) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x1b2a) },

	/* Corsair Void Surround */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a30) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a31) },

	/* Corsair Void Pro Wireless */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a14) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a16) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a1a) },

	/* Corsair Void Pro USB */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a17) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a1d) },

	/* Corsair Void Pro Surround */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a18) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a1e) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a1f) },

	/* Corsair Void Elite Wireless */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a51) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a55) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a75) },

	/* Corsair Void Elite USB */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a52) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a56) },

	/* Corsair Void Elite Surround */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a53) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0a57) },

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
