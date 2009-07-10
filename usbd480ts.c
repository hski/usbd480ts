/*
 * USBD480 USB display touchscreen driver
 *
 * Copyright (C) 2009  Henri Skippari
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 USBD480-LQ043 is a 480x272 pixel display with 16 bpp RBG565 colors.

 To use this driver you should be running firmware version 0.5 (2009/05/28) or later. 
 
 Also you need to have the touch interface configured as vendor specific interface 
 and not as HID interface.
*/

/*
TODO:
-error handling
-touch support
-suspend/resume?

*/


#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h> 
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/input.h>

#define USBD480_INTEPDATASIZE 16

#define USBD480_VID	0x16C0
#define USBD480_PID	0x08A6

#define USBD480_GET_DEVICE_DETAILS	0x80
#define USBD480_SET_TOUCH_MODE		0xE2

#define IOCTL_GET_DEVICE_DETAILS 0x20

#define USBD480TOUCH_DEVICE(vid, pid)			\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE | 	\
		USB_DEVICE_ID_MATCH_INT_CLASS |		\
		USB_DEVICE_ID_MATCH_INT_PROTOCOL,	\
	.idVendor = (vid),				\
	.idProduct = (pid),				\
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC,	\
	.bInterfaceProtocol = 0x01

static struct usb_device_id id_table [] = {
	{ USBD480TOUCH_DEVICE(USBD480_VID, USBD480_PID) },
	{ },
};
MODULE_DEVICE_TABLE (usb, id_table);

struct usbd480ts {
	struct usb_device *udev;
	unsigned int width;
	unsigned int height;	
	char device_name[20];	
	struct input_dev *input;
	unsigned char *intdata;
	struct urb *inturb;
	int noreports;
	int pendown;
	int debounce;
	
};

static int process_touch_data(struct usbd480ts *d)
{
/*
	Very experimental stuff here...
	You can test how it behaves with or without the simple debounce
	and also try different parameters
	
	Also consider the touch reporting mode of USBD480. You can try 
	setting it to provide raw samples or then apply some filtering 
	to the values it reports.
*/
	int x, y, z1, z2, pen;
	unsigned Rt;
	
	//d->noreports = 0;
	d->noreports++;

	x = d->intdata[0] | (d->intdata[1]<<8);
	y = d->intdata[2] | (d->intdata[3]<<8);
	z1 = d->intdata[4] | (d->intdata[5]<<8);
	z2 = d->intdata[6] | (d->intdata[7]<<8);
	pen = d->intdata[8];

	x = x>>1;
	y = y>>1;
	z1 = z1>>1;
	z2 = z2>>1;

	if (z1==0)
		z1 = 1;
	
	/* from ads7846 driver */
	/* see also sbaa155a.pdf from TI */
	Rt = z2;
	Rt -= z1;
	Rt *= x;
	Rt *= 750;
	Rt /= z1;
	Rt = (Rt + 2047) >> 12;

	Rt = 1000 - Rt;

	/* check suitable Z values for finger or stylus */
	if ((Rt > 900) || (Rt < 300))
		Rt = 0;
	
	printk(KERN_INFO "x %d, y %d z1 %d z2 %d pen %d Rt %d count %d\n",
		x, y, z1, z2, pen, Rt, d->noreports);

/* some rough max values for the touch panel */
#define TOUCH_MAX_X 1960
#define TOUCH_MIN_X 40
#define TOUCH_MAX_Y 1950
#define TOUCH_MIN_Y 110
/*
	if (((x < TOUCH_MAX_X) && (x > TOUCH_MIN_X) && 
		(y < TOUCH_MAX_Y) && (y > TOUCH_MIN_Y))
		|| (pen==1))
*/
	if (Rt || (pen==1)) {
		
		/*
		if (Rt && (pen==0)) {
			prev_x = x;
			prev_y = y;
		}
		*/


#define DEBOUNCEVALUE 5
#define DEBOUNCEVALUEUP 50

//#define USEDEBOUNCE

/*
	Remove debounce code?
*/

#ifdef USEDEBOUNCE
	/* simple debounce, requires DEBOUNCEVALUE consecutive pen state reports 
	before changing pen state */
	if (pen==1) { /* pen is up according to report */
		if (d->pendown==0) {
			/* no change, restore debounce value */	
			d->debounce = DEBOUNCEVALUE;
		}
		else 
		{
			/* pen state changed to up in report */
			d->debounce--;
			
			if (d->debounce==0)
			{
				/* we got DEBOUNCEVALUE consecutive pen up reports */
				d->pendown = 0;
				input_report_abs(d->input, ABS_PRESSURE, Rt);
				input_report_key(d->input, BTN_TOUCH, 0);
				input_sync(d->input);
			}
		}
	}
	else /* pen is down according to report */
	{
		if (d->pendown==1) {
			/* no change, restore debounce value */
			d->debounce = DEBOUNCEVALUE;
			
			/* report new position */
			if (Rt) {
				input_report_abs(d->input, ABS_X, x);
				input_report_abs(d->input, ABS_Y, y);
				input_report_abs(d->input, ABS_PRESSURE, Rt);
				input_report_key(d->input, BTN_TOUCH, 1);
				input_sync(d->input);
			}
		}
		else 
		{
			/* pen state changed to down in report */			
			d->debounce--;
			
			if (d->debounce==0) {
				/* we got DEBOUNCEVALUE consecutive pen down reports */
				if (Rt) {
					d->pendown = 1;
					input_report_abs(d->input, ABS_X, x);
					input_report_abs(d->input, ABS_Y, y);
					input_report_abs(d->input, ABS_PRESSURE, Rt);
					input_report_key(d->input, BTN_TOUCH, 1);
					input_sync(d->input);
				}
			}
		}
	}
#else	
	/* no debounce */
	if (pen==1) {
		input_report_abs(d->input, ABS_PRESSURE, Rt);
		input_report_key(d->input, BTN_TOUCH, 0);
	}
	else 
	{
		input_report_abs(d->input, ABS_X, x);
		input_report_abs(d->input, ABS_Y, y);
		input_report_abs(d->input, ABS_PRESSURE, Rt);
		input_report_key(d->input, BTN_TOUCH, 1);
	}
	input_sync(d->input);
	
#endif /* USEDEBOUNCE */	

	}
	
	return 0;
}

static void usbd480tsint_complete(struct urb *urb)
{
	struct usbd480ts *d = urb->context;
	int status = urb->status;
	int retval;

	switch (status) {
	case 0:
		/* success */
		dbg("%s - urb success: %d",
		__func__, urb->status);
		/*
		printk(KERN_ERR "usbd480fb: success "
		"len %d, actual len is %d\n",
		USBD480_INTEPDATASIZE, d->inturb->actual_length);
		*/
		break;
	case -ETIME:
		/* this urb is timing out */
		dbg("%s - urb timed out - was the device unplugged?",
		__func__);
		return;
	case -EOVERFLOW:
		printk(KERN_ERR "usbd480fb: OVERFLOW with data "
		"length %d, actual length is %d\n",
		USBD480_INTEPDATASIZE, d->inturb->actual_length);
	case -ECONNRESET:
	/*case -NOENT:*/
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d",
		__func__, urb->status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d",
		__func__, urb->status);
		goto exit;
	}
	
	process_touch_data(d);	
	
exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		err("%s - usb_submit_urb failed with result: %d",
		__func__, retval);
}

static int usbd480ts_get_device_details(struct usbd480ts *dev)
{
// TODO: return value handling

	int result;
	unsigned char *buffer;

	buffer = kmalloc(64, GFP_KERNEL);
	if (!buffer) {
		dev_err(&dev->udev->dev, "out of memory\n");
		return 0;
	}

	result = usb_control_msg(dev->udev,
				usb_rcvctrlpipe(dev->udev, 0),
				USBD480_GET_DEVICE_DETAILS,
				USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
				0,
				0,
				buffer,	
				64,
				1000);
	if (result)
		dev_dbg(&dev->udev->dev, "result = %d\n", result);

	dev->width = (unsigned char)buffer[20] | ((unsigned char)buffer[21]<<8);
	dev->height = (unsigned char)buffer[22] | ((unsigned char)buffer[23]<<8);
	strncpy(dev->device_name, buffer, 20);
	kfree(buffer);	

	return 0;
}

static int usbd480ts_set_touch_mode(struct usbd480ts *dev, unsigned int mode)	
{
// TODO: return value handling, check valid dev?
	
	int result;

	result = usb_control_msg(dev->udev,
				usb_sndctrlpipe(dev->udev, 0),
				USBD480_SET_TOUCH_MODE,
				USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
				mode,
				0,
				NULL,	
				0,
				1000);
	if (result)
		dev_dbg(&dev->udev->dev, "result = %d\n", result);
						
	return 0;							
}

static ssize_t show_name(struct device *dev, struct device_attribute *attr, char *buf)		
{						
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usbd480ts*d = usb_get_intfdata(intf);
									
	return sprintf(buf, "%s\n", d->device_name);			
}

static DEVICE_ATTR(name, S_IRUGO, show_name, NULL);


/*
static long usbd480ts_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{

	return 0;
}

static struct file_operations usbd480ts_fops = {
	.owner =	THIS_MODULE,
//	.read =		usbd480ts_read,
//	.write =	usbd480ts_write,
	.unlocked_ioctl = usbd480ts_ioctl,
//	.open =		usbd480ts_open,
//	.release =	usbd480ts_release,
};

static struct usb_class_driver usbd480ts_class = {
	.name =		"usbd480ts",
	.fops =		&usbd480ts_fops,
	.minor_base =	USBD480_MINOR_BASE,
};
*/

static int usbd480ts_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usbd480ts *dev = NULL;
	int retval = -ENOMEM;

	struct input_dev *input;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;

	iface_desc = interface->cur_altsetting;
	endpoint = &iface_desc->endpoint[0].desc;

	dev = kzalloc(sizeof(struct usbd480ts), GFP_KERNEL);
	if (dev == NULL) {
		retval = -ENOMEM;
		dev_err(&interface->dev, "Out of memory\n");
		goto error_dev;
	}

	dev->udev = usb_get_dev(udev);
	usb_set_intfdata (interface, dev);

	dev->inturb = usb_alloc_urb(0, GFP_KERNEL);
	if (dev->inturb == NULL) {
		retval = -ENOMEM;
		dev_err(&interface->dev, "Allocating URB failed");
		goto error_touch;
	}

	dev->intdata = usb_buffer_alloc(udev, USBD480_INTEPDATASIZE,
				GFP_KERNEL, &dev->inturb->transfer_dma);
	if (dev->intdata == NULL) {
		retval = -ENOMEM;
		dev_err(&interface->dev, "Allocating URB buffer failed");
		goto error_touch;
	}

	usb_fill_int_urb(dev->inturb, udev, 
		usb_rcvintpipe(udev, endpoint->bEndpointAddress),
		dev->intdata, USBD480_INTEPDATASIZE,
		usbd480tsint_complete, dev, 5 /*endpoint->bInterval*/);
		/*
		endpoint->bInterval
		1 - 4000 updates/second
		4 - 1000 updates/second
		5 - 500 updates/second
		*/
				

/*
	retval = usb_register_dev(interface, &usbd480ts_class);
	if (retval) {
		err("Not able to get a minor for this device.");
		return -ENOMEM;
	}
*/

	retval = device_create_file(&interface->dev, &dev_attr_name);
	if (retval)
		goto error_dev_attr;

	dev_info(&interface->dev, "usbd480ts attached\n");

	usbd480ts_get_device_details(dev);
	
	/*
	Enable touch reporting mode 4. This mode does some internal filtering 
	and only sends reports when the pen is down. You could also try touch 
	mode 2 for example which provides raw output without any filtering.	
	If using mode 4 it might be useful to also tweak the 
	TOUCH_DEBOUNCE_VALUE and TOUCH_PRESSURE_LIMIT_LO and 
	TOUCH_PRESSURE_LIMIT_HI parameters using the SET_CONFIG_VALUE request.
	*/	
	usbd480ts_set_touch_mode(dev, 4);	

	input = input_allocate_device();
	if (!input) {
		dev_err(&interface->dev, "failed to allocate input device.\n");
		goto error_input_alloc;
	}

	dev->input = input;
	
	input->name = "USBD480 touchscreen";
	input->phys = "usbd480/input0"; //TODO: count connected usbd480 devices

	input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	
	input_set_abs_params(input, ABS_X, 0, 4095, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, 4095, 0, 0);
	//input_set_abs_params(input, ABS_PRESSURE, min, max, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE, 0, 4095, 0, 0);

	retval = input_register_device(input);
	if (retval)
		goto error_input_reg;

	dev_info(&interface->dev, "USBD480 touchscreen registered\n");
	
	dev->noreports = 0;
	dev->pendown = 0;
	dev->debounce = DEBOUNCEVALUE;
	
	// TODO: move this to a separate input_open() function
	if (usb_submit_urb(dev->inturb, GFP_KERNEL)) {
		retval = -EIO;
		dev_err(&interface->dev, "Submitting URB failed");
		//goto error_touch;
	}	

	return 0;

//input_unregister_device(dev->input);
error_input_reg:
	input_free_device(dev->input);
error_input_alloc:

error_dev_attr:
	device_remove_file(&interface->dev, &dev_attr_name);
error_touch:
	if (dev->inturb) {
		usb_kill_urb(dev->inturb);
		if (dev->intdata)
			usb_buffer_free(udev, USBD480_INTEPDATASIZE,
				dev->intdata, dev->inturb->transfer_dma);
		usb_free_urb(dev->inturb);
	}	
error_dev:
	usb_set_intfdata(interface, NULL);
	if (dev)
		kfree(dev);

	printk(KERN_INFO "usbd480ts: error probe\n");
	return retval;
}

static void usbd480ts_disconnect(struct usb_interface *interface)
{
	struct usbd480ts *dev;

	dev = usb_get_intfdata (interface);

	//usb_deregister_dev(interface, &usbd480ts_class);

	device_remove_file(&interface->dev, &dev_attr_name);

	if (dev->inturb) {
		usb_kill_urb(dev->inturb);
		if (dev->intdata)
			usb_buffer_free(dev->udev, USBD480_INTEPDATASIZE,
				dev->intdata, dev->inturb->transfer_dma);
		usb_free_urb(dev->inturb);
	}	

	input_unregister_device(dev->input);
	input_free_device(dev->input);

	usb_set_intfdata(interface, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);
	dev_info(&interface->dev, "usbd480ts disconnected\n");
}

static struct usb_driver usbd480ts_driver = {
	.name =		"usbd480ts",
	.probe =	usbd480ts_probe,
	.disconnect =	usbd480ts_disconnect,
	.id_table =	id_table,
};

static int __init usbd480ts_init(void)
{
	int retval = 0;

	retval = usb_register(&usbd480ts_driver);
	if (retval)
		err("usb_register failed. Error number %d", retval);
	return retval;
}

static void __exit usbd480ts_exit(void)
{
	usb_deregister(&usbd480ts_driver);
}

module_init (usbd480ts_init);
module_exit (usbd480ts_exit);

MODULE_AUTHOR("Henri Skippari");
MODULE_DESCRIPTION("USBD480 touchscreen driver");
MODULE_LICENSE("GPL");
