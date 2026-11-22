#ifdef CONFIG_C5_QDSS_PUBLIC_REQUEST_DIAGNOSTIC
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static atomic_t c5_qdss_live_contexts = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_functions = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_requests = ATOMIC_INIT(0);

static int c5_qdss_count_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d", atomic_read((const atomic_t *)kp->arg));
}

static const struct kernel_param_ops c5_qdss_count_ops = {
	.get = c5_qdss_count_get,
};
module_param_cb(c5_live_contexts, &c5_qdss_count_ops,
	&c5_qdss_live_contexts, 0400);
module_param_cb(c5_live_functions, &c5_qdss_count_ops,
	&c5_qdss_live_functions, 0400);
module_param_cb(c5_live_requests, &c5_qdss_count_ops, &c5_qdss_live_requests, 0400);


static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/


/* Each endpoint request permanently owns its endpoint, pool and context.
 * Global reference lock never nests inside the state lock. Queue admission
 * nests state -> UDC; the qualified UDC drops its lock before giveback.
 */
struct c5_qdss_request {
 struct f_qdss *qdss;
 struct usb_ep *ep;
 struct list_head *pool;
 struct qdss_request *client;
 enum qdss_state event;
 bool pending, completing, canceling, cancel_tried, zlp;
};

static void c5_qdss_get_ref(struct f_qdss *q)
{
 unsigned long flags;
 spin_lock_irqsave(&qdss_lock, flags);
 WARN_ON(!q->owners);
 q->owners++;
 spin_unlock_irqrestore(&qdss_lock, flags);
}

static void c5_qdss_put_ref(struct f_qdss *q)
{
 unsigned long flags;
 bool release;
 spin_lock_irqsave(&qdss_lock, flags);
 release = !--q->owners;
 if (release)
  list_del(&q->ch.list);
 spin_unlock_irqrestore(&qdss_lock, flags);
 if (release)
  schedule_work(&q->release_w);
}

static struct f_qdss *c5_qdss_get_channel(struct usb_qdss_ch *ch, bool user)
{
 struct usb_qdss_ch *item;
 struct f_qdss *q, *found = NULL;
 unsigned long flags;
 /* Compare the address against live entries without dereferencing ch. */
 spin_lock_irqsave(&qdss_lock, flags);
 list_for_each_entry(item, &usb_qdss_ch_list, list) {
  if (item != ch)
   continue;
  q = container_of(item, struct f_qdss, ch);
  spin_lock(&q->lock);
  if (q->client_owned && q->c5_test_client && item->priv_usb == q &&
      (!user || (!q->client_closing && item->app_conn))) {
   q->owners++;
   if (user)
    q->request_users++;
   found = q;
  }
  spin_unlock(&q->lock);
  break;
 }
 spin_unlock_irqrestore(&qdss_lock, flags);
 return found;
}

static void c5_qdss_put_user(struct f_qdss *q)
{
 unsigned long flags;
 spin_lock_irqsave(&q->lock, flags);
 q->request_users--;
 spin_unlock_irqrestore(&q->lock, flags);
 wake_up_all(&q->request_wait);
 c5_qdss_put_ref(q);
}

static bool c5_qdss_requests_quiet(struct f_qdss *q)
{
 unsigned long flags;
 bool quiet;
 spin_lock_irqsave(&q->lock, flags);
 quiet = list_empty(&q->request_inflight) && !q->request_callbacks &&
         !q->request_users;
 spin_unlock_irqrestore(&q->lock, flags);
 return quiet;
}

static void c5_qdss_request_complete(struct usb_ep *ep, struct usb_request *req)
{
 struct c5_qdss_request *x = req->context;
 struct f_qdss *q = x->qdss;
 struct qdss_request *client;
 void (*notify)(void *, unsigned int, struct qdss_request *, struct usb_qdss_ch *);
 void *priv;
 enum qdss_state event;
 unsigned long flags;
 int ret;
 c5_qdss_get_ref(q);
 spin_lock_irqsave(&q->lock, flags);
 WARN_ON(ep != x->ep || !x->pending);
 q->request_callbacks++;
 x->pending = false;
 x->completing = true;
 client = x->client;
 event = x->event;
 if (!x->zlp) {
  client->actual = req->actual;
  client->status = req->status;
 } else if (req->status) {
  client->status = req->status;
 }
 if (!x->zlp && !req->status && event != USB_QDSS_CTRL_READ_DONE &&
     req->length && req->length >= ep->maxpacket &&
     !(req->length % ep->maxpacket)) {
  if (q->usb_connected && !q->client_closing && q->ch.app_conn) {
   req->length = 0;
   x->zlp = true;
   x->pending = true;
   ret = usb_ep_queue(x->ep, req, GFP_ATOMIC);
   if (!ret) {
    x->completing = false;
    q->request_callbacks--;
    spin_unlock_irqrestore(&q->lock, flags);
    wake_up_all(&q->request_wait);
    c5_qdss_put_ref(q);
    return;
   }
   x->pending = false;
   client->status = ret;
  } else {
   client->status = -ESHUTDOWN;
  }
 }
 notify = q->ch.notify;
 priv = q->ch.priv;
 spin_unlock_irqrestore(&q->lock, flags);
 /* Only the admitted inert owned callback is qualified, including cancel.
  * The request remains inflight until the notification returns.
  */
 if (notify)
  notify(priv, event, client, NULL);
 spin_lock_irqsave(&q->lock, flags);
 x->client = NULL;
 x->completing = false;
 q->request_callbacks--;
 if (!x->canceling)
  list_move_tail(&req->list, x->pool);
 spin_unlock_irqrestore(&q->lock, flags);
 /* Idle publication permits another CPU to free req and x. */
 wake_up_all(&q->request_wait);
 c5_qdss_put_ref(q);
}

static void qdss_write_complete(struct usb_ep *ep, struct usb_request *req)
{
 c5_qdss_request_complete(ep, req);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep, struct usb_request *req)
{
 c5_qdss_request_complete(ep, req);
}

static void qdss_free_pool(struct usb_ep *ep, struct list_head *pool)
{
 struct usb_request *req, *tmp;
 struct c5_qdss_request *x;
 list_for_each_entry_safe(req, tmp, pool, list) {
  x = req->context;
  list_del(&req->list);
  usb_ep_free_request(x->ep, req);
  atomic_dec(&c5_qdss_live_requests);
  c5_qdss_put_ref(x->qdss);
  kfree(x);
 }
}

static void c5_qdss_free_idle(struct f_qdss *q)
{
 unsigned long flags;
 LIST_HEAD(idle);
 spin_lock_irqsave(&q->lock, flags);
 list_splice_tail_init(&q->data_write_pool, &idle);
 list_splice_tail_init(&q->ctrl_write_pool, &idle);
 list_splice_tail_init(&q->ctrl_read_pool, &idle);
 spin_unlock_irqrestore(&q->lock, flags);
 qdss_free_pool(NULL, &idle);
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
 struct f_qdss *q = c5_qdss_get_channel(ch, true);
 if (!q)
  return;
 c5_qdss_free_idle(q);
 c5_qdss_put_user(q);
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf, int no_read_buf)
{
 struct f_qdss *q = c5_qdss_get_channel(ch, true);
 struct usb_request *req;
 struct c5_qdss_request *x;
 struct usb_ep *in, *out;
 struct list_head *pool;
 unsigned long flags;
 bool debug;
 int i, ret = 0;
 LIST_HEAD(writes);
 LIST_HEAD(reads);
 if (!q)
  return -ENODEV;
 spin_lock_irqsave(&q->lock, flags);
 debug = q->debug_inface_enabled;
 in = debug ? q->port.ctrl_in : q->port.data;
 out = q->port.ctrl_out;
 pool = debug ? &q->ctrl_write_pool : &q->data_write_pool;
 if (!q->usb_connected || q->client_closing || !in || (no_read_buf && !out))
  ret = -ENOTCONN;
 else if (no_write_buf <= 0 || no_write_buf > 2 ||
          (debug ? no_read_buf <= 0 || no_read_buf > 2 : no_read_buf != 0))
  ret = -EINVAL;
 spin_unlock_irqrestore(&q->lock, flags);
 if (ret)
  goto done;
 for (i = 0; i < no_write_buf + no_read_buf; i++) {
  x = kzalloc(sizeof(*x), GFP_ATOMIC);
  if (!x) {
   ret = -ENOMEM;
   goto fail;
  }
  x->qdss = q;
  x->ep = i < no_write_buf ? in : out;
  x->pool = i < no_write_buf ? pool : &q->ctrl_read_pool;
  x->event = i >= no_write_buf ? USB_QDSS_CTRL_READ_DONE :
             (debug ? USB_QDSS_CTRL_WRITE_DONE : USB_QDSS_DATA_WRITE_DONE);
  req = usb_ep_alloc_request(x->ep, GFP_ATOMIC);
  if (!req) {
   kfree(x);
   ret = -ENOMEM;
   goto fail;
  }
  c5_qdss_get_ref(q);
  atomic_inc(&c5_qdss_live_requests);
  req->context = x;
  req->complete = i < no_write_buf ? qdss_write_complete : qdss_ctrl_read_complete;
  list_add_tail(&req->list, i < no_write_buf ? &writes : &reads);
 }
 spin_lock_irqsave(&q->lock, flags);
 if (!q->usb_connected || q->client_closing || debug != q->debug_inface_enabled ||
     in != (debug ? q->port.ctrl_in : q->port.data) || out != q->port.ctrl_out) {
  ret = -ESHUTDOWN;
  spin_unlock_irqrestore(&q->lock, flags);
  goto fail;
 }
 list_splice_tail_init(&writes, pool);
 list_splice_tail_init(&reads, &q->ctrl_read_pool);
 spin_unlock_irqrestore(&q->lock, flags);
 goto done;
fail:
 qdss_free_pool(NULL, &writes);
 qdss_free_pool(NULL, &reads);
done:
 c5_qdss_put_user(q);
 return ret;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
 struct f_qdss *qdss = func_to_qdss(f);
 if (qdss->port.ctrl_in) {
  usb_ep_autoconfig_release(qdss->port.ctrl_in);
  qdss->port.ctrl_in = NULL;
 }
 if (qdss->port.ctrl_out) {
  usb_ep_autoconfig_release(qdss->port.ctrl_out);
  qdss->port.ctrl_out = NULL;
 }
 if (qdss->port.data) {
  usb_ep_autoconfig_release(qdss->port.data);
  qdss->port.data = NULL;
 }
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
 (void)gadget;
 usb_free_all_descriptors(f);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget;
	struct f_qdss *qdss;
	struct usb_ep *ep;
	int iface, id, ret = -ENODEV;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC) &&
     !IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
		return -EOPNOTSUPP;
	gadget = c->cdev->gadget;
	qdss = func_to_qdss(f);
	if (READ_ONCE(qdss->endpoint_error))
		return READ_ONCE(qdss->endpoint_error);
	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;
 qdss->gadget = gadget;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	qdss_data_ep_comp_desc.bMaxBurst =
  !strcmp(qdss->ch.name, USB_QDSS_CH_MDM) ? 0 : 1;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

 qdss_fs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_hs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_fs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_hs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_fs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 qdss_hs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 ret = usb_assign_descriptors(f,
  qdss->debug_inface_enabled ? qdss_fs_desc : qdss_fs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_hs_desc : qdss_hs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_ss_desc : qdss_ss_data_only_desc,
  NULL);
 if (ret)
  goto fail;

 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return ret;
}


static void qdss_disable(struct usb_function *f);
static void qdss_put_pm(struct f_qdss *qdss);

static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

 qdss_disable(f);
	flush_workqueue(qdss->wq);
 if (READ_ONCE(qdss->endpoint_error))
  return;
 qdss_put_pm(qdss);
 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_put_pm(struct f_qdss *qdss)
{
 unsigned long flags;
 bool held;
 spin_lock_irqsave(&qdss->lock, flags);
 held = qdss->pm_owned;
 qdss->pm_owned = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (held)
  usb_gadget_autopm_put_async(qdss->gadget);
}

/* A void composite unbind cannot refuse outer function release. Hold one
 * context reference on the first failed endpoint stop; its PM/claims and
 * descriptors remain quarantined until reboot, never reported as cleanup.
 */
static void c5_qdss_endpoint_failed(struct f_qdss *qdss, int error)
{
 unsigned long flags;
 bool already;
 c5_qdss_get_ref(qdss);
 spin_lock_irqsave(&qdss->lock, flags);
 already = qdss->endpoint_error != 0;
 if (!already)
  qdss->endpoint_error = error;
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (already)
  c5_qdss_put_ref(qdss);
}

static int qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	int ret = READ_ONCE(qdss->endpoint_error);
	if (ret)
		return ret;

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		ret = usb_ep_disable(qdss->port.ctrl_in);
		if (ret)
			goto fail;
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		ret = usb_ep_disable(qdss->port.ctrl_out);
		if (ret)
			goto fail;
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		ret = usb_ep_disable(qdss->port.data);
		if (ret)
			goto fail;
		qdss->data_enabled = 0;
	}
 return 0;
fail:
 c5_qdss_endpoint_failed(qdss, ret);
 return ret;
}

/* Test-client admission is serialized with closing before any work is queued. */
static void c5_qdss_queue_test_work(struct f_qdss *qdss,
                                  struct work_struct *work)
{
 unsigned long flags;
 spin_lock_irqsave(&qdss->lock, flags);
 if (qdss->c5_test_client && qdss->ch.app_conn && !qdss->client_closing)
  queue_work(qdss->wq, work);
 spin_unlock_irqrestore(&qdss->lock, flags);
}

static void c5_qdss_notify_test(struct f_qdss *qdss, unsigned int event)
{
 void (*notify)(void *, unsigned int, struct qdss_request *,
                struct usb_qdss_ch *);
 void *priv;
 unsigned long flags;
 spin_lock_irqsave(&qdss->lock, flags);
 if (!qdss->c5_test_client || !qdss->ch.app_conn || qdss->client_closing) {
  spin_unlock_irqrestore(&qdss->lock, flags);
  return;
 }
 notify = qdss->ch.notify;
 priv = qdss->ch.priv;
 spin_unlock_irqrestore(&qdss->lock, flags);
 /* Only the inert owned callback is admitted; close drains both workers. */
 if (notify)
  notify(priv, event, NULL, event == USB_QDSS_CONNECT ? &qdss->ch : NULL);
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  c5_qdss_notify_test(qdss, USB_QDSS_DISCONNECT);
  return;
 }
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	qdss_put_pm(qdss);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected && !qdss->data_enabled &&
     !qdss->ctrl_in_enabled && !qdss->ctrl_out_enabled && !qdss->pm_owned) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	if (qdss_eps_disable(f))
		return;
	if (IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC)) {
  if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
   c5_qdss_queue_test_work(qdss, &qdss->disconnect_w);
  qdss_put_pm(qdss);
 } else
  queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  if (READ_ONCE(qdss->usb_connected))
   c5_qdss_notify_test(qdss, USB_QDSS_CONNECT);
  return;
 }

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
 unsigned int alt)
{
 struct f_qdss *qdss = func_to_qdss(f);
 struct usb_gadget *gadget = f->config->cdev->gadget;
 unsigned long flags;
 int ret, stop;
 if (READ_ONCE(qdss->endpoint_error))
  return READ_ONCE(qdss->endpoint_error);
 if (alt || (intf != qdss->data_iface_id &&
     (!qdss->debug_inface_enabled || intf != qdss->ctrl_iface_id)))
  return -EINVAL;
 if (gadget->speed != USB_SPEED_SUPER && gadget->speed != USB_SPEED_HIGH)
  return -EINVAL;
 qdss->gadget = gadget;
 if (intf == qdss->data_iface_id && !qdss->data_enabled) {
  ret = usb_gadget_autopm_get_async(gadget);
  if (ret)
   goto fail;
  spin_lock_irqsave(&qdss->lock, flags);
  qdss->pm_owned = true;
  spin_unlock_irqrestore(&qdss->lock, flags);
  ret = config_ep_by_speed(gadget, f, qdss->port.data);
  if (ret)
   goto fail;
  ret = usb_ep_enable(qdss->port.data);
  if (ret)
   goto fail;
  qdss->port.data->driver_data = qdss;
  qdss->data_enabled = 1;
 }
 if (qdss->debug_inface_enabled && intf == qdss->ctrl_iface_id) {
  if (!qdss->ctrl_in_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_in);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_in);
   if (ret)
    goto fail;
   qdss->port.ctrl_in->driver_data = qdss;
   qdss->ctrl_in_enabled = 1;
  }
  if (!qdss->ctrl_out_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_out);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_out);
   if (ret)
    goto fail;
   qdss->port.ctrl_out->driver_data = qdss;
   qdss->ctrl_out_enabled = 1;
  }
 }
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = qdss->data_enabled &&
  (!qdss->debug_inface_enabled ||
   (qdss->ctrl_in_enabled && qdss->ctrl_out_enabled));
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  if (READ_ONCE(qdss->usb_connected))
   c5_qdss_queue_test_work(qdss, &qdss->connect_w);
 } else if (qdss->usb_connected && qdss->ch.app_conn)
  queue_work(qdss->wq, &qdss->connect_w);
 return 0;
fail:
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 stop = qdss_eps_disable(f);
 if (stop)
  return stop;
 qdss_put_pm(qdss);
 return ret;
}


enum qdss_owner { QDSS_INSTANCE, QDSS_CLIENT, QDSS_FUNCTION };

static bool *qdss_owner_flag(struct f_qdss *qdss, enum qdss_owner owner)
{
	if (owner == QDSS_INSTANCE)
		return &qdss->instance_owned;
	if (owner == QDSS_CLIENT)
		return &qdss->client_owned;
	return &qdss->function_allocated;
}

static void qdss_release_context(struct work_struct *work)
{
	struct f_qdss *qdss = container_of(work, struct f_qdss, release_w);

	/* Never drain an owned workqueue from its own callback or a spinlock. */
	destroy_workqueue(qdss->wq);
	atomic_dec(&c5_qdss_live_contexts);
	kfree(qdss->ch.name);
	kfree(qdss);
}

static void qdss_put_owner(struct f_qdss *qdss, enum qdss_owner owner)
{
	unsigned long flags;
	bool release = false;

	spin_lock_irqsave(&qdss_lock, flags);
	if (WARN_ON(!*qdss_owner_flag(qdss, owner) || !qdss->owners)) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
	*qdss_owner_flag(qdss, owner) = false;
	if (owner == QDSS_FUNCTION)
		atomic_dec(&c5_qdss_live_functions);
	if (!--qdss->owners) {
		list_del(&qdss->ch.list);
		release = true;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (release)
		schedule_work(&qdss->release_w);
}

/* Caller holds qdss_lock. */
static struct f_qdss *qdss_find_owner(const char *name, enum qdss_owner owner)
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;

	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (strcmp(name, ch->name))
			continue;
		qdss = container_of(ch, struct f_qdss, ch);
		if (READ_ONCE(qdss->endpoint_error))
			return ERR_PTR(READ_ONCE(qdss->endpoint_error));
		if (*qdss_owner_flag(qdss, owner))
			return ERR_PTR(-EEXIST);
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners++;
		return qdss;
	}
	return ERR_PTR(-ENOENT);
}

static struct f_qdss *qdss_get_owner(const char *name, enum qdss_owner owner)
{
	struct f_qdss *qdss, *found;
	unsigned long flags;

	if (!name || !*name)
		return ERR_PTR(-EINVAL);
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (PTR_ERR(found) != -ENOENT)
		return found;

	qdss = kzalloc(sizeof(*qdss), GFP_KERNEL);
	if (!qdss)
		return ERR_PTR(-ENOMEM);
	qdss->ch.name = kstrdup(name, GFP_KERNEL);
	if (!qdss->ch.name) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	qdss->wq = create_singlethread_workqueue(name);
	if (!qdss->wq) {
		kfree(qdss->ch.name);
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_LIST_HEAD(&qdss->request_inflight);
	init_waitqueue_head(&qdss->request_wait);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);
	INIT_WORK(&qdss->release_w, qdss_release_context);

	/* Recheck after sleeping allocation; exactly one unique name is published. */
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	if (PTR_ERR(found) == -ENOENT) {
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners = 1;
		atomic_inc(&c5_qdss_live_contexts);
		list_add_tail(&qdss->ch.list, &usb_qdss_ch_list);
		spin_unlock_irqrestore(&qdss_lock, flags);
		return qdss;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	destroy_workqueue(qdss->wq);
	kfree(qdss->ch.name);
	kfree(qdss);
	return found;
}



static int c5_qdss_submit(struct usb_qdss_ch *ch, struct qdss_request *client,
 enum qdss_state event)
{
 struct f_qdss *q = c5_qdss_get_channel(ch, true);
 struct usb_request *req;
 struct c5_qdss_request *x;
 struct list_head *pool;
 unsigned long flags;
 int ret;
 if (!q)
  return -ENODEV;
 spin_lock_irqsave(&q->lock, flags);
 if (!client || !client->buf || client->length <= 0 || client->length > 32) {
  ret = -EINVAL;
  goto out;
 }
 if (!q->usb_connected || q->client_closing || !q->ch.app_conn) {
  ret = -ESHUTDOWN;
  goto out;
 }
 if ((event == USB_QDSS_DATA_WRITE_DONE) == q->debug_inface_enabled) {
  ret = -EOPNOTSUPP;
  goto out;
 }
 pool = event == USB_QDSS_DATA_WRITE_DONE ? &q->data_write_pool :
        (event == USB_QDSS_CTRL_WRITE_DONE ? &q->ctrl_write_pool : &q->ctrl_read_pool);
 if (list_empty(pool)) {
  ret = -EAGAIN;
  goto out;
 }
 req = list_first_entry(pool, struct usb_request, list);
 x = req->context;
 req->buf = client->buf;
 req->length = client->length;
 x->client = client;
 x->pending = true;
 x->cancel_tried = false;
 x->zlp = false;
 list_move_tail(&req->list, &q->request_inflight);
 ret = usb_ep_queue(x->ep, req, GFP_ATOMIC);
 if (ret) {
  x->pending = false;
  x->client = NULL;
  list_move_tail(&req->list, x->pool);
 }
out:
 spin_unlock_irqrestore(&q->lock, flags);
 c5_qdss_put_user(q);
 return ret;
}

int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
 return c5_qdss_submit(ch, d_req, USB_QDSS_CTRL_READ_DONE);
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
 return c5_qdss_submit(ch, d_req, USB_QDSS_CTRL_WRITE_DONE);
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
 return c5_qdss_submit(ch, d_req, USB_QDSS_DATA_WRITE_DONE);
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *, unsigned int, struct qdss_request *,
		struct usb_qdss_ch *))
{
	struct f_qdss *qdss;
	struct usb_qdss_ch *ch;
	unsigned long flags;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC))
		return ERR_PTR(-EOPNOTSUPP);
	if (!notify)
		return NULL;
	qdss = qdss_get_owner(name, QDSS_CLIENT);
	if (IS_ERR(qdss))
		return ERR_CAST(qdss);
	ch = &qdss->ch;
	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	if (qdss->usb_connected)
		queue_work(qdss->wq, &qdss->connect_w);
	spin_unlock_irqrestore(&qdss_lock, flags);
	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);


/* The diagnostic owns only a fixed payload. Endpoint requests belong to the
 * actual public pools. The root probe mutex serializes prepare/release/close;
 * completion never takes that mutex and publishes pending=false last.
 */
enum { C5_QDSS_TRANSFER_BYTES = 32 };
struct c5_qdss_transfer {
 spinlock_t lock;
 struct qdss_request request;
 unsigned char *bytes;
 enum qdss_state event;
 bool pending;
 unsigned int completed, actual, in_bytes, out_bytes;
 int status;
};

static struct c5_qdss_transfer c5_test_transfer = {
 .lock = __SPIN_LOCK_UNLOCKED(c5_test_transfer.lock),
};

static void c5_qdss_transfer_complete(struct qdss_request *req, unsigned int event)
{
 struct c5_qdss_transfer *x = &c5_test_transfer;
 unsigned long flags;
 if (req != &x->request)
  return;
 spin_lock_irqsave(&x->lock, flags);
 x->status = (!x->pending || !x->bytes || event != x->event) ? -EIO : req->status;
 x->actual = req->actual;
 x->completed++;
 x->pending = false;
 spin_unlock_irqrestore(&x->lock, flags);
 /* No request/buffer access after publishing completion. */
}

static int c5_qdss_transfer_release(struct c5_qdss_transfer *x)
{
 unsigned long flags;
 unsigned char *bytes;
 spin_lock_irqsave(&x->lock, flags);
 if (x->pending) {
  spin_unlock_irqrestore(&x->lock, flags);
  return -EBUSY;
 }
 bytes = x->bytes;
 x->bytes = NULL;
 x->request.buf = NULL;
 spin_unlock_irqrestore(&x->lock, flags);
 kfree(bytes);
 return 0;
}

static int c5_qdss_transfer_prepare(struct c5_qdss_transfer *x, enum qdss_state event)
{
 unsigned long flags;
 unsigned char *bytes;
 unsigned int i;
 if (x->bytes || x->pending)
  return -EBUSY;
 bytes = kmalloc(C5_QDSS_TRANSFER_BYTES, GFP_KERNEL);
 if (!bytes)
  return -ENOMEM;
 for (i = 0; i < C5_QDSS_TRANSFER_BYTES; i++)
  bytes[i] = event == USB_QDSS_CTRL_READ_DONE ? 0x5a : (unsigned char)(0xa0 + i);
 spin_lock_irqsave(&x->lock, flags);
 x->bytes = bytes;
 x->request.buf = (char *)bytes;
 x->request.length = C5_QDSS_TRANSFER_BYTES;
 x->request.actual = 0;
 x->request.status = -EINPROGRESS;
 x->event = event;
 x->completed = 0;
 x->actual = 0;
 x->status = -EINPROGRESS;
 x->pending = false;
 spin_unlock_irqrestore(&x->lock, flags);
 return 0;
}

static int c5_qdss_transfer_queue(struct c5_qdss_transfer *x, struct usb_qdss_ch *ch)
{
 unsigned long flags;
 int ret;
 spin_lock_irqsave(&x->lock, flags);
 if (!x->bytes || x->pending || x->completed) {
  spin_unlock_irqrestore(&x->lock, flags);
  return -EBUSY;
 }
 if (x->event == USB_QDSS_CTRL_READ_DONE) {
  if (x->out_bytes >= C5_QDSS_TRANSFER_BYTES) {
   spin_unlock_irqrestore(&x->lock, flags);
   return -EDQUOT;
  }
  x->out_bytes += C5_QDSS_TRANSFER_BYTES;
 } else {
  if (x->in_bytes >= 2 * C5_QDSS_TRANSFER_BYTES) {
   spin_unlock_irqrestore(&x->lock, flags);
   return -EDQUOT;
  }
  x->in_bytes += C5_QDSS_TRANSFER_BYTES;
 }
 /* Count attempted submissions conservatively, including a rejection.
  * The caller stops on error; this function never retries a payload.
  */
 x->pending = true;
 spin_unlock_irqrestore(&x->lock, flags);
 if (x->event == USB_QDSS_DATA_WRITE_DONE)
  ret = usb_qdss_write(ch, &x->request);
 else if (x->event == USB_QDSS_CTRL_WRITE_DONE)
  ret = usb_qdss_ctrl_write(ch, &x->request);
 else
  ret = usb_qdss_ctrl_read(ch, &x->request);
 if (ret) {
  spin_lock_irqsave(&x->lock, flags);
  x->pending = false;
  x->status = ret;
  spin_unlock_irqrestore(&x->lock, flags);
 }
 return ret;
}

static int c5_qdss_drain_requests(struct f_qdss *q)
{
 struct usb_request *req, *candidate;
 struct c5_qdss_request *x;
 unsigned long flags;
 int ret;
 for (;;) {
  candidate = NULL;
  spin_lock_irqsave(&q->lock, flags);
  list_for_each_entry(req, &q->request_inflight, list) {
   x = req->context;
   if (x->pending && !x->cancel_tried && !x->completing) {
    candidate = req;
    x->cancel_tried = true;
    x->canceling = true;
    break;
   }
  }
  spin_unlock_irqrestore(&q->lock, flags);
  if (!candidate)
   break;
  x = candidate->context;
  /* canceling pins the request across synchronous or racing giveback. */
  ret = usb_ep_dequeue(x->ep, candidate);
  spin_lock_irqsave(&q->lock, flags);
  x->canceling = false;
  if (!x->pending && !x->completing)
   list_move_tail(&candidate->list, x->pool);
  if (ret && x->pending) {
   q->request_error = ret;
   spin_unlock_irqrestore(&q->lock, flags);
   return ret;
  }
  spin_unlock_irqrestore(&q->lock, flags);
 }
 /* The condition takes only the state lock; wakeups occur after releasing it.
  * Selected wait.h releases its waitqueue lock before evaluating condition.
  */
 if (!wait_event_timeout(q->request_wait, c5_qdss_requests_quiet(q),
                         msecs_to_jiffies(1000))) {
  spin_lock_irqsave(&q->lock, flags);
  ret = q->request_error ? q->request_error : -ETIMEDOUT;
  q->request_error = ret;
  spin_unlock_irqrestore(&q->lock, flags);
  return ret;
 }
 spin_lock_irqsave(&q->lock, flags);
 q->request_error = 0;
 spin_unlock_irqrestore(&q->lock, flags);
 return 0;
}

static int c5_qdss_close_test_client(struct f_qdss *qdss)
{
 unsigned long flags;
 int ret;
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->client_closing = true;
 qdss->ch.app_conn = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 ret = READ_ONCE(qdss->endpoint_error);
 if (ret)
  return ret;
 cancel_work_sync(&qdss->connect_w);
 cancel_work_sync(&qdss->disconnect_w);
 ret = c5_qdss_drain_requests(qdss);
 if (ret)
  return ret;
 ret = READ_ONCE(qdss->endpoint_error);
 if (ret)
  return ret;
 c5_qdss_free_idle(qdss);
 ret = c5_qdss_transfer_release(&c5_test_transfer);
 if (ret)
  return ret;
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->ch.priv_usb = NULL;
 qdss->ch.notify = NULL;
 qdss->ch.priv = NULL;
 qdss->c5_test_client = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_put_owner(qdss, QDSS_CLIENT);
 return 0;
}

void usb_qdss_close(struct usb_qdss_ch *ch)
{
 struct f_qdss *q = c5_qdss_get_channel(ch, false);
 unsigned long flags;
 int ret;
 if (!q)
  return;
 ret = c5_qdss_close_test_client(q);
 spin_lock_irqsave(&q->lock, flags);
 q->request_error = ret;
 spin_unlock_irqrestore(&q->lock, flags);
 c5_qdss_put_ref(q);
}
EXPORT_SYMBOL(usb_qdss_close);

#ifdef CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC
static DEFINE_MUTEX(c5_qdss_test_lock);
static struct usb_qdss_ch *c5_qdss_test_ch;
static atomic_t c5_test_connect = ATOMIC_INIT(0);
static atomic_t c5_test_disconnect = ATOMIC_INIT(0);
static unsigned int c5_test_opens, c5_test_closes, c5_test_duplicates;
static unsigned int c5_test_allocated, c5_test_freed, c5_test_requests;

static void c5_qdss_test_notify(void *priv, unsigned int event,
 struct qdss_request *req, struct usb_qdss_ch *ch)
{
 if (event == USB_QDSS_CONNECT)
  atomic_inc(&c5_test_connect);
 else if (event == USB_QDSS_DISCONNECT)
  atomic_inc(&c5_test_disconnect);
 else if (req)
  c5_qdss_transfer_complete(req, event);
}

static int c5_qdss_client_probe_set(const char *val, const struct kernel_param *kp)
{
 struct f_qdss *q;
 unsigned long flags;
 unsigned int command, count;
 int ret;
 ret = kstrtouint(val, 0, &command);
 if (ret)
  return ret;
 if (command < 1 || command > 5)
  return -EINVAL;
 mutex_lock(&c5_qdss_test_lock);
 ret = 0;
 if (command == 1) {
  if (c5_qdss_test_ch) {
   ret = -EBUSY;
   goto out;
  }
  q = qdss_get_owner(USB_QDSS_CH_MDM, QDSS_CLIENT);
  if (IS_ERR(q)) {
   ret = PTR_ERR(q);
   goto out;
  }
  spin_lock_irqsave(&c5_test_transfer.lock, flags);
  c5_test_transfer.in_bytes = 0;
  c5_test_transfer.out_bytes = 0;
  spin_unlock_irqrestore(&c5_test_transfer.lock, flags);
  spin_lock_irqsave(&q->lock, flags);
  q->client_closing = false;
  q->request_error = 0;
  q->c5_test_client = true;
  q->ch.priv_usb = q;
  q->ch.priv = NULL;
  q->ch.notify = c5_qdss_test_notify;
  q->ch.app_conn = 1;
  c5_qdss_test_ch = &q->ch;
  spin_unlock_irqrestore(&q->lock, flags);
  c5_test_opens++;
  if (READ_ONCE(q->usb_connected))
   c5_qdss_queue_test_work(q, &q->connect_w);
  goto out;
 }
 if (!c5_qdss_test_ch) {
  ret = -ENODEV;
  goto out;
 }
 q = c5_qdss_test_ch->priv_usb;
 if (command == 2) {
  if (c5_test_requests) {
   ret = -EBUSY;
   goto out;
  }
  if (!READ_ONCE(q->usb_connected)) {
   ret = -ENOTCONN;
   goto out;
  }
  count = q->debug_inface_enabled ? 4 : 2;
  ret = usb_qdss_alloc_req(c5_qdss_test_ch, 2, q->debug_inface_enabled ? 2 : 0);
  if (!ret) {
   c5_test_requests = count;
   c5_test_allocated += count;
  }
 } else if (command == 3) {
  if (!c5_qdss_requests_quiet(q)) {
   ret = -EBUSY;
   goto out;
  }
  usb_qdss_free_req(c5_qdss_test_ch);
  c5_test_freed += c5_test_requests;
  c5_test_requests = 0;
 } else if (command == 4) {
  c5_qdss_get_ref(q);
  usb_qdss_close(c5_qdss_test_ch);
  spin_lock_irqsave(&q->lock, flags);
  ret = q->request_error;
  spin_unlock_irqrestore(&q->lock, flags);
  c5_qdss_put_ref(q);
  if (ret)
   goto out;
  c5_qdss_test_ch = NULL;
  c5_test_freed += c5_test_requests;
  c5_test_requests = 0;
  c5_test_closes++;
 } else {
  q = qdss_get_owner(USB_QDSS_CH_MDM, QDSS_CLIENT);
  if (IS_ERR(q)) {
   ret = PTR_ERR(q) == -EEXIST ? 0 : PTR_ERR(q);
   if (!ret)
    c5_test_duplicates++;
  } else {
   qdss_put_owner(q, QDSS_CLIENT);
   ret = -EIO;
  }
 }
out:
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}

static int c5_qdss_client_probe_get(char *buffer, const struct kernel_param *kp)
{
 int ret;
 mutex_lock(&c5_qdss_test_lock);
 ret = scnprintf(buffer, PAGE_SIZE,
  "open=%u close=%u duplicate=%u connect=%d disconnect=%d allocated=%u freed=%u requests=%u client=%u\n",
  c5_test_opens, c5_test_closes, c5_test_duplicates,
  atomic_read(&c5_test_connect), atomic_read(&c5_test_disconnect),
  c5_test_allocated, c5_test_freed, c5_test_requests, !!c5_qdss_test_ch);
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}
static const struct kernel_param_ops c5_qdss_client_probe_ops = {
 .set = c5_qdss_client_probe_set,
 .get = c5_qdss_client_probe_get,
};
module_param_cb(c5_client_probe, &c5_qdss_client_probe_ops, NULL, 0600);

/* Caller serializes client open/close, allocation and transfer commands. */
static int c5_qdss_transfer_probe_set(const char *value, const struct kernel_param *kp)
{
 unsigned int command;
 struct f_qdss *q;
 enum qdss_state event;
 unsigned long flags;
 int ret = kstrtouint(value, 0, &command);
 if (ret)
  return ret;
 if (command < 1 || command > 3)
  return -EINVAL;
 mutex_lock(&c5_qdss_test_lock);
 if (!c5_qdss_test_ch) {
  ret = -ENODEV;
  goto out;
 }
 if (command == 3) {
  ret = c5_qdss_transfer_release(&c5_test_transfer);
  goto out;
 }
 q = c5_qdss_test_ch->priv_usb;
 spin_lock_irqsave(&q->lock, flags);
 event = command == 2 ? USB_QDSS_CTRL_READ_DONE :
         (q->debug_inface_enabled ? USB_QDSS_CTRL_WRITE_DONE : USB_QDSS_DATA_WRITE_DONE);
 ret = q->usb_connected && !q->client_closing && q->c5_test_client ? 0 : -ENOTCONN;
 if (!ret && command == 2 && !q->debug_inface_enabled)
  ret = -EOPNOTSUPP;
 spin_unlock_irqrestore(&q->lock, flags);
 if (ret)
  goto out;
 ret = c5_qdss_transfer_prepare(&c5_test_transfer, event);
 if (ret)
  goto out;
 ret = c5_qdss_transfer_queue(&c5_test_transfer, c5_qdss_test_ch);
 if (ret)
  c5_qdss_transfer_release(&c5_test_transfer);
out:
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}

static int c5_qdss_transfer_probe_get(char *buffer, const struct kernel_param *kp)
{
 unsigned long flags;
 unsigned int pending,completed,actual,owned,match=0,i,event,in_bytes,out_bytes;
 int status,ret;
 mutex_lock(&c5_qdss_test_lock);
 spin_lock_irqsave(&c5_test_transfer.lock, flags);
 pending=c5_test_transfer.pending;
 completed=c5_test_transfer.completed;
 actual=c5_test_transfer.actual;
 status=c5_test_transfer.status;
 owned=!!c5_test_transfer.bytes;
 event=c5_test_transfer.event;
 in_bytes=c5_test_transfer.in_bytes;
 out_bytes=c5_test_transfer.out_bytes;
 if (owned && !pending && completed && !status && actual==C5_QDSS_TRANSFER_BYTES) {
  match=1;
  for(i=0;i<C5_QDSS_TRANSFER_BYTES;i++)
   if(c5_test_transfer.bytes[i]!=(unsigned char)(0xa0+i))
    match=0;
 }
 spin_unlock_irqrestore(&c5_test_transfer.lock, flags);
 ret=scnprintf(buffer,PAGE_SIZE,
  "owned=%u pending=%u completed=%u status=%d actual=%u match=%u buffer=%u event=%u in_bytes=%u out_bytes=%u\n",
  owned,pending,completed,status,actual,match,owned,event,in_bytes,out_bytes);
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}
static const struct kernel_param_ops c5_qdss_transfer_probe_ops = {
 .set=c5_qdss_transfer_probe_set,
 .get=c5_qdss_transfer_probe_get,
};
module_param_cb(c5_transfer_probe,&c5_qdss_transfer_probe_ops,NULL,0600);

#endif


static void qdss_cleanup(void)
{
	unsigned long flags;

	/* Referenced channels must not be forcibly freed during unregister. */
	spin_lock_irqsave(&qdss_lock, flags);
	WARN_ON(!list_empty(&usb_qdss_ch_list));
	spin_unlock_irqrestore(&qdss_lock, flags);
}

static void qdss_free_func(struct usb_function *f)
{
	qdss_put_owner(func_to_qdss(f), QDSS_FUNCTION);
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	bool enabled;

	if (!qdss)
		return -ENODEV;
	spin_lock_irqsave(&qdss_lock, flags);
	enabled = qdss->debug_inface_enabled;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return snprintf(page, PAGE_SIZE, "%s\n", enabled ? "Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (!qdss)
		return -ENODEV;
	if (!page || kstrtou8(page, 0, &stats) || stats > 1)
		return -EINVAL;
	spin_lock_irqsave(&qdss_lock, flags);
	if (qdss->function_allocated || qdss->client_owned || qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return -EBUSY;
	}
	qdss->debug_inface_enabled = stats == 1;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	if (opts->usb_qdss)
		qdss_put_owner(opts->usb_qdss, QDSS_INSTANCE);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	struct f_qdss *qdss;

	if (opts->usb_qdss)
		return -EBUSY;
	if (strlen(name) + 1 > 15)
		return -ENAMETOOLONG;
	qdss = qdss_get_owner(name, QDSS_INSTANCE);
	if (IS_ERR(qdss))
		return PTR_ERR(qdss);
	opts->channel_name = (char *)qdss->ch.name;
	opts->usb_qdss = qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;
	unsigned long flags;

	if (!usb_qdss)
		return ERR_PTR(-ENODEV);
	spin_lock_irqsave(&qdss_lock, flags);
	if (READ_ONCE(usb_qdss->endpoint_error)) {
		int error = READ_ONCE(usb_qdss->endpoint_error);
		spin_unlock_irqrestore(&qdss_lock, flags);
		return ERR_PTR(error);
	}
	if (usb_qdss->function_allocated) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return ERR_PTR(-EBUSY);
	}
	usb_qdss->function_allocated = true;
	usb_qdss->owners++;
	atomic_inc(&c5_qdss_live_functions);
	spin_unlock_irqrestore(&qdss_lock, flags);

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");


#else
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
#ifdef CONFIG_C5_QDSS_FIXED_TRANSFER_DIAGNOSTIC
#ifdef CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC
#ifdef CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static atomic_t c5_qdss_live_contexts = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_functions = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_requests = ATOMIC_INIT(0);

static int c5_qdss_count_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d", atomic_read((const atomic_t *)kp->arg));
}

static const struct kernel_param_ops c5_qdss_count_ops = {
	.get = c5_qdss_count_get,
};
module_param_cb(c5_live_contexts, &c5_qdss_count_ops,
	&c5_qdss_live_contexts, 0400);
module_param_cb(c5_live_functions, &c5_qdss_count_ops,
	&c5_qdss_live_functions, 0400);
module_param_cb(c5_live_requests, &c5_qdss_count_ops, &c5_qdss_live_requests, 0400);


static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

static void qdss_free_pool(struct usb_ep *ep, struct list_head *pool)
{
 struct usb_request *req, *tmp;
 list_for_each_entry_safe(req, tmp, pool, list) {
  list_del(&req->list);
  usb_ep_free_request(ep, req);
  atomic_dec(&c5_qdss_live_requests);
 }
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
 struct f_qdss *qdss;
 unsigned long flags;
 LIST_HEAD(data);
 LIST_HEAD(writes);
 LIST_HEAD(reads);
 if (!ch || !(qdss = ch->priv_usb))
  return;
 spin_lock_irqsave(&qdss->lock, flags);
 list_splice_init(&qdss->data_write_pool, &data);
 list_splice_init(&qdss->ctrl_write_pool, &writes);
 list_splice_init(&qdss->ctrl_read_pool, &reads);
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_free_pool(qdss->port.data, &data);
 qdss_free_pool(qdss->port.ctrl_in, &writes);
 qdss_free_pool(qdss->port.ctrl_out, &reads);
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf, int no_read_buf)
{
 struct f_qdss *qdss;
 struct usb_request *req;
 struct usb_ep *in;
 struct list_head *pool;
 unsigned long flags;
 int i;
 LIST_HEAD(writes);
 LIST_HEAD(reads);
 if (!ch || !(qdss = ch->priv_usb))
  return -ENODEV;
 if (no_write_buf <= 0 || (qdss->debug_inface_enabled ? no_read_buf <= 0 : no_read_buf != 0))
  return -EINVAL;
 in = qdss->debug_inface_enabled ? qdss->port.ctrl_in : qdss->port.data;
 pool = qdss->debug_inface_enabled ? &qdss->ctrl_write_pool : &qdss->data_write_pool;
 if (!in || (no_read_buf && !qdss->port.ctrl_out))
  return -ENODEV;
 for (i = 0; i < no_write_buf; i++) {
  req = usb_ep_alloc_request(in, GFP_ATOMIC);
  if (!req)
   goto fail;
  atomic_inc(&c5_qdss_live_requests);
  req->complete = qdss_write_complete;
  list_add_tail(&req->list, &writes);
 }
 for (i = 0; i < no_read_buf; i++) {
  req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
  if (!req)
   goto fail;
  atomic_inc(&c5_qdss_live_requests);
  req->complete = qdss_ctrl_read_complete;
  list_add_tail(&req->list, &reads);
 }
 spin_lock_irqsave(&qdss->lock, flags);
 list_splice_tail_init(&writes, pool);
 list_splice_tail_init(&reads, &qdss->ctrl_read_pool);
 spin_unlock_irqrestore(&qdss->lock, flags);
 return 0;
fail:
 qdss_free_pool(in, &writes);
 qdss_free_pool(qdss->port.ctrl_out, &reads);
 return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
 struct f_qdss *qdss = func_to_qdss(f);
 if (qdss->port.ctrl_in) {
  usb_ep_autoconfig_release(qdss->port.ctrl_in);
  qdss->port.ctrl_in = NULL;
 }
 if (qdss->port.ctrl_out) {
  usb_ep_autoconfig_release(qdss->port.ctrl_out);
  qdss->port.ctrl_out = NULL;
 }
 if (qdss->port.data) {
  usb_ep_autoconfig_release(qdss->port.data);
  qdss->port.data = NULL;
 }
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
 (void)gadget;
 usb_free_all_descriptors(f);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget;
	struct f_qdss *qdss;
	struct usb_ep *ep;
	int iface, id, ret = -ENODEV;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC) &&
     !IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
		return -EOPNOTSUPP;
	gadget = c->cdev->gadget;
	qdss = func_to_qdss(f);
	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;
 qdss->gadget = gadget;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	qdss_data_ep_comp_desc.bMaxBurst =
  !strcmp(qdss->ch.name, USB_QDSS_CH_MDM) ? 0 : 1;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

 qdss_fs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_hs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_fs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_hs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_fs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 qdss_hs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 ret = usb_assign_descriptors(f,
  qdss->debug_inface_enabled ? qdss_fs_desc : qdss_fs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_hs_desc : qdss_hs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_ss_desc : qdss_ss_data_only_desc,
  NULL);
 if (ret)
  goto fail;

 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return ret;
}


static void qdss_disable(struct usb_function *f);
static void qdss_put_pm(struct f_qdss *qdss);

static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

 qdss_disable(f);
	flush_workqueue(qdss->wq);
 qdss_put_pm(qdss);
 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_put_pm(struct f_qdss *qdss)
{
 unsigned long flags;
 bool held;
 spin_lock_irqsave(&qdss->lock, flags);
 held = qdss->pm_owned;
 qdss->pm_owned = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (held)
  usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

/* Test-client admission is serialized with closing before any work is queued. */
static void c5_qdss_queue_test_work(struct f_qdss *qdss,
                                  struct work_struct *work)
{
 unsigned long flags;
 spin_lock_irqsave(&qdss->lock, flags);
 if (qdss->c5_test_client && qdss->ch.app_conn && !qdss->client_closing)
  queue_work(qdss->wq, work);
 spin_unlock_irqrestore(&qdss->lock, flags);
}

static void c5_qdss_notify_test(struct f_qdss *qdss, unsigned int event)
{
 void (*notify)(void *, unsigned int, struct qdss_request *,
                struct usb_qdss_ch *);
 void *priv;
 unsigned long flags;
 spin_lock_irqsave(&qdss->lock, flags);
 if (!qdss->c5_test_client || !qdss->ch.app_conn || qdss->client_closing) {
  spin_unlock_irqrestore(&qdss->lock, flags);
  return;
 }
 notify = qdss->ch.notify;
 priv = qdss->ch.priv;
 spin_unlock_irqrestore(&qdss->lock, flags);
 /* Only the inert owned callback is admitted; close drains both workers. */
 if (notify)
  notify(priv, event, NULL, event == USB_QDSS_CONNECT ? &qdss->ch : NULL);
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  c5_qdss_notify_test(qdss, USB_QDSS_DISCONNECT);
  return;
 }
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	qdss_put_pm(qdss);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected && !qdss->data_enabled &&
     !qdss->ctrl_in_enabled && !qdss->ctrl_out_enabled && !qdss->pm_owned) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	if (IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC)) {
  if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
   c5_qdss_queue_test_work(qdss, &qdss->disconnect_w);
  qdss_put_pm(qdss);
 } else
  queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  if (READ_ONCE(qdss->usb_connected))
   c5_qdss_notify_test(qdss, USB_QDSS_CONNECT);
  return;
 }

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
 unsigned int alt)
{
 struct f_qdss *qdss = func_to_qdss(f);
 struct usb_gadget *gadget = f->config->cdev->gadget;
 unsigned long flags;
 int ret;
 if (alt || (intf != qdss->data_iface_id &&
     (!qdss->debug_inface_enabled || intf != qdss->ctrl_iface_id)))
  return -EINVAL;
 if (gadget->speed != USB_SPEED_SUPER && gadget->speed != USB_SPEED_HIGH)
  return -EINVAL;
 qdss->gadget = gadget;
 if (intf == qdss->data_iface_id && !qdss->data_enabled) {
  ret = usb_gadget_autopm_get_async(gadget);
  if (ret)
   goto fail;
  spin_lock_irqsave(&qdss->lock, flags);
  qdss->pm_owned = true;
  spin_unlock_irqrestore(&qdss->lock, flags);
  ret = config_ep_by_speed(gadget, f, qdss->port.data);
  if (ret)
   goto fail;
  ret = usb_ep_enable(qdss->port.data);
  if (ret)
   goto fail;
  qdss->port.data->driver_data = qdss;
  qdss->data_enabled = 1;
 }
 if (qdss->debug_inface_enabled && intf == qdss->ctrl_iface_id) {
  if (!qdss->ctrl_in_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_in);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_in);
   if (ret)
    goto fail;
   qdss->port.ctrl_in->driver_data = qdss;
   qdss->ctrl_in_enabled = 1;
  }
  if (!qdss->ctrl_out_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_out);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_out);
   if (ret)
    goto fail;
   qdss->port.ctrl_out->driver_data = qdss;
   qdss->ctrl_out_enabled = 1;
  }
 }
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = qdss->data_enabled &&
  (!qdss->debug_inface_enabled ||
   (qdss->ctrl_in_enabled && qdss->ctrl_out_enabled));
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  if (READ_ONCE(qdss->usb_connected))
   c5_qdss_queue_test_work(qdss, &qdss->connect_w);
 } else if (qdss->usb_connected && qdss->ch.app_conn)
  queue_work(qdss->wq, &qdss->connect_w);
 return 0;
fail:
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_eps_disable(f);
 qdss_put_pm(qdss);
 return ret;
}


enum qdss_owner { QDSS_INSTANCE, QDSS_CLIENT, QDSS_FUNCTION };

static bool *qdss_owner_flag(struct f_qdss *qdss, enum qdss_owner owner)
{
	if (owner == QDSS_INSTANCE)
		return &qdss->instance_owned;
	if (owner == QDSS_CLIENT)
		return &qdss->client_owned;
	return &qdss->function_allocated;
}

static void qdss_release_context(struct work_struct *work)
{
	struct f_qdss *qdss = container_of(work, struct f_qdss, release_w);

	/* Never drain an owned workqueue from its own callback or a spinlock. */
	destroy_workqueue(qdss->wq);
	atomic_dec(&c5_qdss_live_contexts);
	kfree(qdss->ch.name);
	kfree(qdss);
}

static void qdss_put_owner(struct f_qdss *qdss, enum qdss_owner owner)
{
	unsigned long flags;
	bool release = false;

	spin_lock_irqsave(&qdss_lock, flags);
	if (WARN_ON(!*qdss_owner_flag(qdss, owner) || !qdss->owners)) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
	*qdss_owner_flag(qdss, owner) = false;
	if (owner == QDSS_FUNCTION)
		atomic_dec(&c5_qdss_live_functions);
	if (!--qdss->owners) {
		list_del(&qdss->ch.list);
		release = true;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (release)
		schedule_work(&qdss->release_w);
}

/* Caller holds qdss_lock. */
static struct f_qdss *qdss_find_owner(const char *name, enum qdss_owner owner)
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;

	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (strcmp(name, ch->name))
			continue;
		qdss = container_of(ch, struct f_qdss, ch);
		if (*qdss_owner_flag(qdss, owner))
			return ERR_PTR(-EEXIST);
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners++;
		return qdss;
	}
	return ERR_PTR(-ENOENT);
}

static struct f_qdss *qdss_get_owner(const char *name, enum qdss_owner owner)
{
	struct f_qdss *qdss, *found;
	unsigned long flags;

	if (!name || !*name)
		return ERR_PTR(-EINVAL);
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (PTR_ERR(found) != -ENOENT)
		return found;

	qdss = kzalloc(sizeof(*qdss), GFP_KERNEL);
	if (!qdss)
		return ERR_PTR(-ENOMEM);
	qdss->ch.name = kstrdup(name, GFP_KERNEL);
	if (!qdss->ch.name) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	qdss->wq = create_singlethread_workqueue(name);
	if (!qdss->wq) {
		kfree(qdss->ch.name);
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);
	INIT_WORK(&qdss->release_w, qdss_release_context);

	/* Recheck after sleeping allocation; exactly one unique name is published. */
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	if (PTR_ERR(found) == -ENOENT) {
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners = 1;
		atomic_inc(&c5_qdss_live_contexts);
		list_add_tail(&qdss->ch.list, &usb_qdss_ch_list);
		spin_unlock_irqrestore(&qdss_lock, flags);
		return qdss;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	destroy_workqueue(qdss->wq);
	kfree(qdss->ch.name);
	kfree(qdss);
	return found;
}


int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
  return -EOPNOTSUPP;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
  return -EOPNOTSUPP;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
  return -EOPNOTSUPP;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *, unsigned int, struct qdss_request *,
		struct usb_qdss_ch *))
{
	struct f_qdss *qdss;
	struct usb_qdss_ch *ch;
	unsigned long flags;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC))
		return ERR_PTR(-EOPNOTSUPP);
	if (!notify)
		return NULL;
	qdss = qdss_get_owner(name, QDSS_CLIENT);
	if (IS_ERR(qdss))
		return ERR_CAST(qdss);
	ch = &qdss->ch;
	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	if (qdss->usb_connected)
		queue_work(qdss->wq, &qdss->connect_w);
	spin_unlock_irqrestore(&qdss_lock, flags);
	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

/* Private bounded synthetic request owner. Caller serializes submit/release.
 * Completion never takes that caller mutex. Retain everything on cancel error.
 * This file is not selected by a phone image or a public provider.
 */
enum { C5_QDSS_TRANSFER_BYTES = 32 };
struct c5_qdss_transfer {
 struct usb_ep *ep;
 struct usb_request *req;
 spinlock_t lock;
 bool pending;
 unsigned int completed;
 int status;
 unsigned int actual;
 unsigned char *bytes;
};

static void c5_qdss_transfer_complete(struct usb_ep *ep, struct usb_request *req)
{
 struct c5_qdss_transfer *x = req->context;
 unsigned long flags;
 spin_lock_irqsave(&x->lock, flags);
 x->status = req->status;
 x->actual = req->actual;
 x->completed++;
 x->pending = false;
 spin_unlock_irqrestore(&x->lock, flags);
 /* No access to x or req after publication: the owner may release now. */
}

static int c5_qdss_transfer_prepare(struct c5_qdss_transfer *x, struct usb_ep *ep)
{
 struct usb_request *req;
 unsigned char *bytes;
 unsigned int i;
 if (!ep)
  return -ENODEV;
 if (x->req || x->bytes)
  return -EBUSY;
 bytes = kmalloc(C5_QDSS_TRANSFER_BYTES, GFP_KERNEL);
 if (!bytes)
  return -ENOMEM;
 req = usb_ep_alloc_request(ep, GFP_KERNEL);
 if (!req) {
  kfree(bytes);
  return -ENOMEM;
 }
 atomic_inc(&c5_qdss_live_requests);
 x->ep = ep;
 x->req = req;
 x->bytes = bytes;
 x->completed = 0;
 x->status = -EINPROGRESS;
 x->actual = 0;
 x->pending = false;
 for (i = 0; i < C5_QDSS_TRANSFER_BYTES; i++)
  x->bytes[i] = (unsigned char)(0xa0 + i);
 req->buf = x->bytes;
 req->length = C5_QDSS_TRANSFER_BYTES;
 req->context = x;
 req->complete = c5_qdss_transfer_complete;
 return 0;
}

/* Caller holds endpoint admission lock; no sleeping allocation here. */
static int c5_qdss_transfer_queue(struct c5_qdss_transfer *x)
{
 unsigned long flags;
 int ret;
 if (!x->req)
  return -ENODEV;
 spin_lock_irqsave(&x->lock, flags);
 if (x->pending || x->completed) {
  spin_unlock_irqrestore(&x->lock, flags);
  return -EBUSY;
 }
 x->pending = true;
 spin_unlock_irqrestore(&x->lock, flags);
 ret = usb_ep_queue(x->ep, x->req, GFP_ATOMIC);
 if (ret) {
  spin_lock_irqsave(&x->lock, flags);
  x->pending = false;
  x->status = ret;
  spin_unlock_irqrestore(&x->lock, flags);
  usb_ep_free_request(x->ep, x->req);
  atomic_dec(&c5_qdss_live_requests);
  kfree(x->bytes);
  x->bytes = NULL;
  x->req = NULL;
  x->ep = NULL;
 }
 return ret;
}

static int c5_qdss_transfer_start(struct c5_qdss_transfer *x, struct usb_ep *ep)
{
 int ret = c5_qdss_transfer_prepare(x, ep);
 return ret ? ret : c5_qdss_transfer_queue(x);
}

static int c5_qdss_transfer_release(struct c5_qdss_transfer *x)
{
 unsigned long flags;
 bool pending;
 int ret = 0;
 if (!x->req)
  return x->bytes ? -EIO : 0;
 spin_lock_irqsave(&x->lock, flags);
 pending = x->pending;
 spin_unlock_irqrestore(&x->lock, flags);
 if (pending)
  ret = usb_ep_dequeue(x->ep, x->req);
 /* The endpoint may complete concurrently with dequeue, including an error
  * return for an already completed request. Only completion releases ownership.
  */
 spin_lock_irqsave(&x->lock, flags);
 pending = x->pending;
 spin_unlock_irqrestore(&x->lock, flags);
 if (pending)
  return ret ? ret : -EIO;
 usb_ep_free_request(x->ep, x->req);
  atomic_dec(&c5_qdss_live_requests);
  kfree(x->bytes);
  x->bytes = NULL;
 x->req = NULL;
 x->ep = NULL;
 return 0;
}

static struct c5_qdss_transfer c5_test_transfer = {
 .lock = __SPIN_LOCK_UNLOCKED(c5_test_transfer.lock),
};

static int c5_qdss_close_test_client(struct f_qdss *qdss)
{
 unsigned long flags;
 int ret = c5_qdss_transfer_release(&c5_test_transfer);
 if (ret)
  return ret;
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->client_closing = true;
 qdss->ch.app_conn = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 cancel_work_sync(&qdss->connect_w);
 cancel_work_sync(&qdss->disconnect_w);
 usb_qdss_free_req(&qdss->ch);
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->ch.priv_usb = NULL;
 qdss->ch.notify = NULL;
 qdss->ch.priv = NULL;
 qdss->c5_test_client = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_put_owner(qdss, QDSS_CLIENT);
 return 0;
}

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	qdss = ch->priv_usb;
	if (!qdss || !qdss->client_owned) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC) && qdss->c5_test_client) {
  spin_unlock_irqrestore(&qdss_lock, flags);
  c5_qdss_close_test_client(qdss);
  return;
 }
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		qdss_put_owner(qdss, QDSS_CLIENT);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
	qdss_put_owner(qdss, QDSS_CLIENT);
}
EXPORT_SYMBOL(usb_qdss_close);

#ifdef CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC
static DEFINE_MUTEX(c5_qdss_test_lock);
static struct usb_qdss_ch *c5_qdss_test_ch;
static atomic_t c5_test_connect = ATOMIC_INIT(0);
static atomic_t c5_test_disconnect = ATOMIC_INIT(0);
static unsigned int c5_test_opens, c5_test_closes, c5_test_duplicates;
static unsigned int c5_test_allocated, c5_test_freed, c5_test_requests;

static void c5_qdss_test_notify(void *priv, unsigned int event,
 struct qdss_request *req, struct usb_qdss_ch *ch)
{
 if (event == USB_QDSS_CONNECT)
  atomic_inc(&c5_test_connect);
 else if (event == USB_QDSS_DISCONNECT)
  atomic_inc(&c5_test_disconnect);
}

static int c5_qdss_client_probe_set(const char *val, const struct kernel_param *kp)
{
 struct f_qdss *q;
 unsigned long flags;
 unsigned int command, count;
 int ret;
 ret = kstrtouint(val, 0, &command);
 if (ret)
  return ret;
 if (command < 1 || command > 5)
  return -EINVAL;
 mutex_lock(&c5_qdss_test_lock);
 ret = 0;
 if (command == 1) {
  if (c5_qdss_test_ch) {
   ret = -EBUSY;
   goto out;
  }
  q = qdss_get_owner(USB_QDSS_CH_MDM, QDSS_CLIENT);
  if (IS_ERR(q)) {
   ret = PTR_ERR(q);
   goto out;
  }
  spin_lock_irqsave(&q->lock, flags);
  q->client_closing = false;
  q->c5_test_client = true;
  q->ch.priv_usb = q;
  q->ch.priv = NULL;
  q->ch.notify = c5_qdss_test_notify;
  q->ch.app_conn = 1;
  c5_qdss_test_ch = &q->ch;
  spin_unlock_irqrestore(&q->lock, flags);
  c5_test_opens++;
  if (READ_ONCE(q->usb_connected))
   c5_qdss_queue_test_work(q, &q->connect_w);
  goto out;
 }
 if (!c5_qdss_test_ch) {
  ret = -ENODEV;
  goto out;
 }
 q = c5_qdss_test_ch->priv_usb;
 if (command == 2) {
  if (c5_test_requests) {
   ret = -EBUSY;
   goto out;
  }
  if (!READ_ONCE(q->usb_connected)) {
   ret = -ENOTCONN;
   goto out;
  }
  count = q->debug_inface_enabled ? 4 : 2;
  ret = usb_qdss_alloc_req(c5_qdss_test_ch, 2, q->debug_inface_enabled ? 2 : 0);
  if (!ret) {
   c5_test_requests = count;
   c5_test_allocated += count;
  }
 } else if (command == 3) {
  usb_qdss_free_req(c5_qdss_test_ch);
  c5_test_freed += c5_test_requests;
  c5_test_requests = 0;
 } else if (command == 4) {
  ret = c5_qdss_close_test_client(q);
  if (ret)
   goto out;
  c5_qdss_test_ch = NULL;
  c5_test_freed += c5_test_requests;
  c5_test_requests = 0;
  c5_test_closes++;
 } else {
  q = qdss_get_owner(USB_QDSS_CH_MDM, QDSS_CLIENT);
  if (IS_ERR(q)) {
   ret = PTR_ERR(q) == -EEXIST ? 0 : PTR_ERR(q);
   if (!ret)
    c5_test_duplicates++;
  } else {
   qdss_put_owner(q, QDSS_CLIENT);
   ret = -EIO;
  }
 }
out:
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}

static int c5_qdss_client_probe_get(char *buffer, const struct kernel_param *kp)
{
 int ret;
 mutex_lock(&c5_qdss_test_lock);
 ret = scnprintf(buffer, PAGE_SIZE,
  "open=%u close=%u duplicate=%u connect=%d disconnect=%d allocated=%u freed=%u requests=%u client=%u\n",
  c5_test_opens, c5_test_closes, c5_test_duplicates,
  atomic_read(&c5_test_connect), atomic_read(&c5_test_disconnect),
  c5_test_allocated, c5_test_freed, c5_test_requests, !!c5_qdss_test_ch);
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}
static const struct kernel_param_ops c5_qdss_client_probe_ops = {
 .set = c5_qdss_client_probe_set,
 .get = c5_qdss_client_probe_get,
};
module_param_cb(c5_client_probe, &c5_qdss_client_probe_ops, NULL, 0600);

/* Caller serializes client open/close, allocation and transfer commands. */
static int c5_qdss_transfer_probe_set(const char *value, const struct kernel_param *kp)
{
 unsigned int command;
 struct f_qdss *q;
 struct usb_ep *ep;
 unsigned long flags;
 int ret = kstrtouint(value, 0, &command);
 if (ret)
  return ret;
 if (command < 1 || command > 3)
  return -EINVAL;
 mutex_lock(&c5_qdss_test_lock);
 if (!c5_qdss_test_ch) {
  ret = -ENODEV;
  goto out;
 }
 q = c5_qdss_test_ch->priv_usb;
 if (command == 3) {
  ret = c5_qdss_transfer_release(&c5_test_transfer);
  goto out;
 }
 spin_lock_irqsave(&q->lock, flags);
 ep = command == 1 ? (q->debug_inface_enabled ? q->port.ctrl_in : q->port.data) : q->port.ctrl_out;
 ret = q->usb_connected && !q->client_closing ? 0 : -ENOTCONN;
 spin_unlock_irqrestore(&q->lock, flags);
 if (ret)
  goto out;
 if (command == 2 && !q->debug_inface_enabled) {
  ret = -EOPNOTSUPP;
  goto out;
 }
 ret = c5_qdss_transfer_prepare(&c5_test_transfer, ep);
 if (ret)
  goto out;
 /* OUT must overwrite distinct bytes; untouched storage cannot prove RX. */
 if (command == 2)
  memset(c5_test_transfer.bytes, 0x5a, C5_QDSS_TRANSFER_BYTES);
 /* Disable first clears usb_connected under this same admission lock.
  * Request allocation may sleep above; queueing below uses GFP_ATOMIC.
  */
 spin_lock_irqsave(&q->lock, flags);
 if (!q->usb_connected || q->client_closing || !q->c5_test_client ||
     ep != (command == 1 ? (q->debug_inface_enabled ? q->port.ctrl_in : q->port.data) : q->port.ctrl_out))
  ret = -ENOTCONN;
 else
  ret = c5_qdss_transfer_queue(&c5_test_transfer);
 spin_unlock_irqrestore(&q->lock, flags);
 if (ret && c5_test_transfer.req)
  c5_qdss_transfer_release(&c5_test_transfer);
out:
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}

static int c5_qdss_transfer_probe_get(char *buffer, const struct kernel_param *kp)
{
 unsigned long flags;
 unsigned int pending,completed,actual,owned,buffer_owned,match=0,i;
 int status,ret;
 mutex_lock(&c5_qdss_test_lock);
 spin_lock_irqsave(&c5_test_transfer.lock, flags);
 pending=c5_test_transfer.pending;
 completed=c5_test_transfer.completed;
 actual=c5_test_transfer.actual;
 status=c5_test_transfer.status;
 owned=!!c5_test_transfer.req;
 buffer_owned=!!c5_test_transfer.bytes;
 if (owned && buffer_owned && !pending && completed && !status && actual==C5_QDSS_TRANSFER_BYTES) {
  match=1;
  for(i=0;i<C5_QDSS_TRANSFER_BYTES;i++)
   if(c5_test_transfer.bytes[i]!=(unsigned char)(0xa0+i))
    match=0;
 }
 spin_unlock_irqrestore(&c5_test_transfer.lock, flags);
 ret=scnprintf(buffer,PAGE_SIZE,"owned=%u pending=%u completed=%u status=%d actual=%u match=%u buffer=%u\n",owned,pending,completed,status,actual,match,buffer_owned);
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}
static const struct kernel_param_ops c5_qdss_transfer_probe_ops = {
 .set=c5_qdss_transfer_probe_set,
 .get=c5_qdss_transfer_probe_get,
};
module_param_cb(c5_transfer_probe,&c5_qdss_transfer_probe_ops,NULL,0600);

#endif


static void qdss_cleanup(void)
{
	unsigned long flags;

	/* Referenced channels must not be forcibly freed during unregister. */
	spin_lock_irqsave(&qdss_lock, flags);
	WARN_ON(!list_empty(&usb_qdss_ch_list));
	spin_unlock_irqrestore(&qdss_lock, flags);
}

static void qdss_free_func(struct usb_function *f)
{
	qdss_put_owner(func_to_qdss(f), QDSS_FUNCTION);
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	bool enabled;

	if (!qdss)
		return -ENODEV;
	spin_lock_irqsave(&qdss_lock, flags);
	enabled = qdss->debug_inface_enabled;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return snprintf(page, PAGE_SIZE, "%s\n", enabled ? "Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (!qdss)
		return -ENODEV;
	if (!page || kstrtou8(page, 0, &stats) || stats > 1)
		return -EINVAL;
	spin_lock_irqsave(&qdss_lock, flags);
	if (qdss->function_allocated || qdss->client_owned || qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return -EBUSY;
	}
	qdss->debug_inface_enabled = stats == 1;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	if (opts->usb_qdss)
		qdss_put_owner(opts->usb_qdss, QDSS_INSTANCE);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	struct f_qdss *qdss;

	if (opts->usb_qdss)
		return -EBUSY;
	if (strlen(name) + 1 > 15)
		return -ENAMETOOLONG;
	qdss = qdss_get_owner(name, QDSS_INSTANCE);
	if (IS_ERR(qdss))
		return PTR_ERR(qdss);
	opts->channel_name = (char *)qdss->ch.name;
	opts->usb_qdss = qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;
	unsigned long flags;

	if (!usb_qdss)
		return ERR_PTR(-ENODEV);
	spin_lock_irqsave(&qdss_lock, flags);
	if (usb_qdss->function_allocated) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return ERR_PTR(-EBUSY);
	}
	usb_qdss->function_allocated = true;
	usb_qdss->owners++;
	atomic_inc(&c5_qdss_live_functions);
	spin_unlock_irqrestore(&qdss_lock, flags);

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#else
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	struct usb_request *req;
	struct list_head *act, *tmp;

	pr_debug("usb_qdss_free_req\n");

	qdss = ch->priv_usb;
	if (!qdss) {
		pr_err("usb_qdss_free_req: qdss ctx is NULL\n");
		return;
	}

	list_for_each_safe(act, tmp, &qdss->data_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.data, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_in, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_read_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_out, req);
	}
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf,
	int no_read_buf)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_request *req;
	struct usb_ep *in;
	struct list_head *list_pool;
	int i;

	pr_debug("usb_qdss_alloc_req\n");

	if (!qdss) {
		pr_err("usb_qdss_alloc_req: channel %s closed\n", ch->name);
		return -ENODEV;
	}

	if ((qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf <= 0)) ||
		(!qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf))) {
		pr_err("usb_qdss_alloc_req: missing params\n");
		return -ENODEV;
	}

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
	}

	for (i = 0; i < no_write_buf; i++) {
		req = usb_ep_alloc_request(in, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req: ctrl_in allocation err\n");
			goto fail;
		}
		req->complete = qdss_write_complete;
		list_add_tail(&req->list, list_pool);
	}

	for (i = 0; i < no_read_buf; i++) {
		req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req:ctrl_out allocation err\n");
			goto fail;
		}
		req->complete = qdss_ctrl_read_complete;
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
	}

	return 0;

fail:
	usb_qdss_free_req(ch);
	return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
	struct f_qdss *qdss = func_to_qdss(f);

	pr_debug("clear_eps\n");

	if (qdss->port.ctrl_in)
		qdss->port.ctrl_in->driver_data = NULL;
	if (qdss->port.ctrl_out)
		qdss->port.ctrl_out->driver_data = NULL;
	if (qdss->port.data)
		qdss->port.data->driver_data = NULL;
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
	pr_debug("clear_desc\n");

	if (gadget_is_superspeed(gadget) && f->ss_descriptors)
		usb_free_descriptors(f->ss_descriptors);

	if (gadget_is_dualspeed(gadget) && f->hs_descriptors)
		usb_free_descriptors(f->hs_descriptors);

	if (f->fs_descriptors)
		usb_free_descriptors(f->fs_descriptors);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget = c->cdev->gadget;
	struct f_qdss *qdss = func_to_qdss(f);
	struct usb_ep *ep;
	int iface, id;

	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		qdss_data_ep_comp_desc.bMaxBurst = 0;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

	/*update fs descriptors*/
	qdss_fs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_fs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_fs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->fs_descriptors = usb_copy_descriptors(qdss_fs_desc);
	} else
		f->fs_descriptors = usb_copy_descriptors(
							qdss_fs_data_only_desc);
	/*update descriptors*/
	qdss_hs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_hs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_hs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->hs_descriptors = usb_copy_descriptors(qdss_hs_desc);
	} else
		f->hs_descriptors = usb_copy_descriptors(
							qdss_hs_data_only_desc);
	if (!f->hs_descriptors) {
		pr_err("usb_copy_descriptors error\n");
		goto fail;
	}

	/* update ss descriptors */
	if (gadget_is_superspeed(gadget)) {
		if (qdss->debug_inface_enabled)
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_desc);
		else
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_data_only_desc);
		if (!f->ss_descriptors) {
			pr_err("usb_copy_descriptors error\n");
			goto fail;
		}
	}

	c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return -ENOTSUPP;
}


static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

	flush_workqueue(qdss->wq);
	c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
				unsigned int alt)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = f->config->cdev->gadget;
	struct usb_qdss_ch *ch = &qdss->ch;
	int ret = 0;

	pr_debug("qdss_set_alt qdss pointer = %pK\n", qdss);
	qdss->gadget = gadget;

	if (alt != 0)
		goto fail1;

	if (gadget->speed != USB_SPEED_SUPER &&
		gadget->speed != USB_SPEED_HIGH) {
		pr_err("qdss_st_alt: qdss supportes HS or SS only\n");
		ret = -EINVAL;
		goto fail1;
	}

	if (intf == qdss->data_iface_id && !qdss->data_enabled) {
		/* Increment usage count on connect */
		usb_gadget_autopm_get_async(qdss->gadget);

		if (config_ep_by_speed(gadget, f, qdss->port.data)) {
			ret = -EINVAL;
			goto fail;
		}

		ret = usb_ep_enable(qdss->port.data);
		if (ret)
			goto fail;

		qdss->port.data->driver_data = qdss;
		qdss->data_enabled = 1;


	} else if ((intf == qdss->ctrl_iface_id) &&
	(qdss->debug_inface_enabled)) {

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_in)) {
			ret = -EINVAL;
			goto fail1;
		}

		ret = usb_ep_enable(qdss->port.ctrl_in);
		if (ret)
			goto fail1;

		qdss->port.ctrl_in->driver_data = qdss;
		qdss->ctrl_in_enabled = 1;

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_out)) {
			ret = -EINVAL;
			goto fail1;
		}


		ret = usb_ep_enable(qdss->port.ctrl_out);
		if (ret)
			goto fail1;

		qdss->port.ctrl_out->driver_data = qdss;
		qdss->ctrl_out_enabled = 1;
	}

	if (qdss->debug_inface_enabled) {
		if (qdss->ctrl_out_enabled && qdss->ctrl_in_enabled &&
			qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF enabled\n");
		}
	} else {
		if (qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF disabled\n");
		}
	}

	if (qdss->usb_connected && ch->app_conn)
		queue_work(qdss->wq, &qdss->connect_w);

	return 0;
fail:
	/* Decrement usage count in case of failure */
	usb_gadget_autopm_put_async(qdss->gadget);
fail1:
	pr_err("qdss_set_alt failed\n");
	qdss_eps_disable(f);
	return ret;
}

static struct f_qdss *alloc_usb_qdss(char *channel_name)
{
	struct f_qdss *qdss;
	int found = 0;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(channel_name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_err("%s: (%s) is already available.\n",
				__func__, channel_name);
		return ERR_PTR(-EEXIST);
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	qdss = kzalloc(sizeof(struct f_qdss), GFP_KERNEL);
	if (!qdss) {
		pr_err("%s: Unable to allocate qdss device\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	qdss->wq = create_singlethread_workqueue(channel_name);
	if (!qdss->wq) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_irqsave(&qdss_lock, flags);
	ch = &qdss->ch;
	ch->name = channel_name;
	list_add_tail(&ch->list, &usb_qdss_ch_list);
	spin_unlock_irqrestore(&qdss_lock, flags);

	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);

	return qdss;
}

int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *priv, unsigned int event,
		struct qdss_request *d_req, struct usb_qdss_ch *))
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;
	unsigned long flags;
	int found = 0;

	pr_debug("usb_qdss_open\n");

	if (!notify) {
		pr_err("usb_qdss_open: notification func is missing\n");
		return NULL;
	}

	spin_lock_irqsave(&qdss_lock, flags);
	/* Check if we already have a channel with this name */
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (!found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_debug("usb_qdss_open failed as %s not found\n", name);
		return NULL;
	}

	pr_debug("usb_qdss_open: qdss ctx found\n");
	qdss = container_of(ch, struct f_qdss, ch);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	spin_unlock_irqrestore(&qdss_lock, flags);

	/* the case USB cabel was connected before qdss called qdss_open */
	if (qdss->usb_connected == 1)
		queue_work(qdss->wq, &qdss->connect_w);

	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_gadget *gadget;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	gadget = qdss->gadget;
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
}
EXPORT_SYMBOL(usb_qdss_close);

static void qdss_cleanup(void)
{
	struct f_qdss *qdss;
	struct list_head *act, *tmp;
	struct usb_qdss_ch *_ch;
	unsigned long flags;

	pr_debug("qdss_cleanup\n");

	list_for_each_safe(act, tmp, &usb_qdss_ch_list) {
		_ch = list_entry(act, struct usb_qdss_ch, list);
		qdss = container_of(_ch, struct f_qdss, ch);
		spin_lock_irqsave(&qdss_lock, flags);
		destroy_workqueue(qdss->wq);
		if (!_ch->priv) {
			list_del(&_ch->list);
			kfree(qdss);
		}
		spin_unlock_irqrestore(&qdss_lock, flags);
	}
}

static void qdss_free_func(struct usb_function *f)
{
	/* Do nothing as usb_qdss_alloc() doesn't alloc anything. */
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	return snprintf(page, PAGE_SIZE, "%s\n",
		(to_f_qdss_opts(item)->usb_qdss->debug_inface_enabled == 1) ?
		"Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (page == NULL) {
		pr_err("Invalid buffer");
		return len;
	}

	if (kstrtou8(page, 0, &stats) != 0 && (stats != 0 || stats != 1)) {
		pr_err("(%u)Wrong value. enter 0 to disable or 1 to enable.\n",
			stats);
		return len;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	qdss->debug_inface_enabled = (stats == 1 ? "true" : "false");
	spin_unlock_irqrestore(&qdss->lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(opts->channel_name, ch->name)) {
			list_del(&ch->list);
			break;
		}
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	kfree(opts->channel_name);
	destroy_workqueue(opts->usb_qdss->wq);
	kfree(opts->usb_qdss);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	char *ptr;
	size_t name_len;
	struct f_qdss *usb_qdss;

	/* get channel_name as expected input qdss.<channel_name> */
	name_len = strlen(name) + 1;
	if (name_len > 15)
		return -ENAMETOOLONG;

	/* get channel name */
	ptr = kstrndup(name, name_len, GFP_KERNEL);
	if (!ptr) {
		pr_err("error:%ld\n", PTR_ERR(ptr));
		return -ENOMEM;
	}

	opts->channel_name = ptr;
	pr_debug("qdss: channel_name:%s\n", opts->channel_name);

	usb_qdss = alloc_usb_qdss(opts->channel_name);
	if (IS_ERR(usb_qdss)) {
		pr_err("Failed to create usb_qdss port(%s)\n",
				opts->channel_name);
		return -ENOMEM;
	}

	opts->usb_qdss = usb_qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#endif

#else
#ifdef CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static atomic_t c5_qdss_live_contexts = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_functions = ATOMIC_INIT(0);

static int c5_qdss_count_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d", atomic_read((const atomic_t *)kp->arg));
}

static const struct kernel_param_ops c5_qdss_count_ops = {
	.get = c5_qdss_count_get,
};
module_param_cb(c5_live_contexts, &c5_qdss_count_ops,
	&c5_qdss_live_contexts, 0400);
module_param_cb(c5_live_functions, &c5_qdss_count_ops,
	&c5_qdss_live_functions, 0400);


static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	struct usb_request *req;
	struct list_head *act, *tmp;

	pr_debug("usb_qdss_free_req\n");

	qdss = ch->priv_usb;
	if (!qdss) {
		pr_err("usb_qdss_free_req: qdss ctx is NULL\n");
		return;
	}

	list_for_each_safe(act, tmp, &qdss->data_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.data, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_in, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_read_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_out, req);
	}
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf,
	int no_read_buf)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_request *req;
	struct usb_ep *in;
	struct list_head *list_pool;
	int i;

	pr_debug("usb_qdss_alloc_req\n");

	if (!qdss) {
		pr_err("usb_qdss_alloc_req: channel %s closed\n", ch->name);
		return -ENODEV;
	}

	if ((qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf <= 0)) ||
		(!qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf))) {
		pr_err("usb_qdss_alloc_req: missing params\n");
		return -ENODEV;
	}

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
	}

	for (i = 0; i < no_write_buf; i++) {
		req = usb_ep_alloc_request(in, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req: ctrl_in allocation err\n");
			goto fail;
		}
		req->complete = qdss_write_complete;
		list_add_tail(&req->list, list_pool);
	}

	for (i = 0; i < no_read_buf; i++) {
		req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req:ctrl_out allocation err\n");
			goto fail;
		}
		req->complete = qdss_ctrl_read_complete;
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
	}

	return 0;

fail:
	usb_qdss_free_req(ch);
	return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
 struct f_qdss *qdss = func_to_qdss(f);
 if (qdss->port.ctrl_in) {
  usb_ep_autoconfig_release(qdss->port.ctrl_in);
  qdss->port.ctrl_in = NULL;
 }
 if (qdss->port.ctrl_out) {
  usb_ep_autoconfig_release(qdss->port.ctrl_out);
  qdss->port.ctrl_out = NULL;
 }
 if (qdss->port.data) {
  usb_ep_autoconfig_release(qdss->port.data);
  qdss->port.data = NULL;
 }
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
 (void)gadget;
 usb_free_all_descriptors(f);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget;
	struct f_qdss *qdss;
	struct usb_ep *ep;
	int iface, id, ret = -ENODEV;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC) &&
     !IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
		return -EOPNOTSUPP;
	gadget = c->cdev->gadget;
	qdss = func_to_qdss(f);
	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;
 qdss->gadget = gadget;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	qdss_data_ep_comp_desc.bMaxBurst =
  !strcmp(qdss->ch.name, USB_QDSS_CH_MDM) ? 0 : 1;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

 qdss_fs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_hs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_fs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_hs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_fs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 qdss_hs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 ret = usb_assign_descriptors(f,
  qdss->debug_inface_enabled ? qdss_fs_desc : qdss_fs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_hs_desc : qdss_hs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_ss_desc : qdss_ss_data_only_desc,
  NULL);
 if (ret)
  goto fail;

 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return ret;
}


static void qdss_disable(struct usb_function *f);
static void qdss_put_pm(struct f_qdss *qdss);

static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

 qdss_disable(f);
	flush_workqueue(qdss->wq);
 qdss_put_pm(qdss);
 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_put_pm(struct f_qdss *qdss)
{
 unsigned long flags;
 bool held;
 spin_lock_irqsave(&qdss->lock, flags);
 held = qdss->pm_owned;
 qdss->pm_owned = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (held)
  usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	qdss_put_pm(qdss);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected && !qdss->data_enabled &&
     !qdss->ctrl_in_enabled && !qdss->ctrl_out_enabled && !qdss->pm_owned) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	if (IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  qdss_put_pm(qdss);
 else
  queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
 unsigned int alt)
{
 struct f_qdss *qdss = func_to_qdss(f);
 struct usb_gadget *gadget = f->config->cdev->gadget;
 unsigned long flags;
 int ret;
 if (alt || (intf != qdss->data_iface_id &&
     (!qdss->debug_inface_enabled || intf != qdss->ctrl_iface_id)))
  return -EINVAL;
 if (gadget->speed != USB_SPEED_SUPER && gadget->speed != USB_SPEED_HIGH)
  return -EINVAL;
 qdss->gadget = gadget;
 if (intf == qdss->data_iface_id && !qdss->data_enabled) {
  ret = usb_gadget_autopm_get_async(gadget);
  if (ret)
   goto fail;
  spin_lock_irqsave(&qdss->lock, flags);
  qdss->pm_owned = true;
  spin_unlock_irqrestore(&qdss->lock, flags);
  ret = config_ep_by_speed(gadget, f, qdss->port.data);
  if (ret)
   goto fail;
  ret = usb_ep_enable(qdss->port.data);
  if (ret)
   goto fail;
  qdss->port.data->driver_data = qdss;
  qdss->data_enabled = 1;
 }
 if (qdss->debug_inface_enabled && intf == qdss->ctrl_iface_id) {
  if (!qdss->ctrl_in_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_in);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_in);
   if (ret)
    goto fail;
   qdss->port.ctrl_in->driver_data = qdss;
   qdss->ctrl_in_enabled = 1;
  }
  if (!qdss->ctrl_out_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_out);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_out);
   if (ret)
    goto fail;
   qdss->port.ctrl_out->driver_data = qdss;
   qdss->ctrl_out_enabled = 1;
  }
 }
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = qdss->data_enabled &&
  (!qdss->debug_inface_enabled ||
   (qdss->ctrl_in_enabled && qdss->ctrl_out_enabled));
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (qdss->usb_connected && qdss->ch.app_conn)
  queue_work(qdss->wq, &qdss->connect_w);
 return 0;
fail:
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_eps_disable(f);
 qdss_put_pm(qdss);
 return ret;
}


enum qdss_owner { QDSS_INSTANCE, QDSS_CLIENT, QDSS_FUNCTION };

static bool *qdss_owner_flag(struct f_qdss *qdss, enum qdss_owner owner)
{
	if (owner == QDSS_INSTANCE)
		return &qdss->instance_owned;
	if (owner == QDSS_CLIENT)
		return &qdss->client_owned;
	return &qdss->function_allocated;
}

static void qdss_release_context(struct work_struct *work)
{
	struct f_qdss *qdss = container_of(work, struct f_qdss, release_w);

	/* Never drain an owned workqueue from its own callback or a spinlock. */
	destroy_workqueue(qdss->wq);
	atomic_dec(&c5_qdss_live_contexts);
	kfree(qdss->ch.name);
	kfree(qdss);
}

static void qdss_put_owner(struct f_qdss *qdss, enum qdss_owner owner)
{
	unsigned long flags;
	bool release = false;

	spin_lock_irqsave(&qdss_lock, flags);
	if (WARN_ON(!*qdss_owner_flag(qdss, owner) || !qdss->owners)) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
	*qdss_owner_flag(qdss, owner) = false;
	if (owner == QDSS_FUNCTION)
		atomic_dec(&c5_qdss_live_functions);
	if (!--qdss->owners) {
		list_del(&qdss->ch.list);
		release = true;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (release)
		schedule_work(&qdss->release_w);
}

/* Caller holds qdss_lock. */
static struct f_qdss *qdss_find_owner(const char *name, enum qdss_owner owner)
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;

	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (strcmp(name, ch->name))
			continue;
		qdss = container_of(ch, struct f_qdss, ch);
		if (*qdss_owner_flag(qdss, owner))
			return ERR_PTR(-EEXIST);
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners++;
		return qdss;
	}
	return ERR_PTR(-ENOENT);
}

static struct f_qdss *qdss_get_owner(const char *name, enum qdss_owner owner)
{
	struct f_qdss *qdss, *found;
	unsigned long flags;

	if (!name || !*name)
		return ERR_PTR(-EINVAL);
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (PTR_ERR(found) != -ENOENT)
		return found;

	qdss = kzalloc(sizeof(*qdss), GFP_KERNEL);
	if (!qdss)
		return ERR_PTR(-ENOMEM);
	qdss->ch.name = kstrdup(name, GFP_KERNEL);
	if (!qdss->ch.name) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	qdss->wq = create_singlethread_workqueue(name);
	if (!qdss->wq) {
		kfree(qdss->ch.name);
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);
	INIT_WORK(&qdss->release_w, qdss_release_context);

	/* Recheck after sleeping allocation; exactly one unique name is published. */
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	if (PTR_ERR(found) == -ENOENT) {
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners = 1;
		atomic_inc(&c5_qdss_live_contexts);
		list_add_tail(&qdss->ch.list, &usb_qdss_ch_list);
		spin_unlock_irqrestore(&qdss_lock, flags);
		return qdss;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	destroy_workqueue(qdss->wq);
	kfree(qdss->ch.name);
	kfree(qdss);
	return found;
}


int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *, unsigned int, struct qdss_request *,
		struct usb_qdss_ch *))
{
	struct f_qdss *qdss;
	struct usb_qdss_ch *ch;
	unsigned long flags;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC))
		return ERR_PTR(-EOPNOTSUPP);
	if (!notify)
		return NULL;
	qdss = qdss_get_owner(name, QDSS_CLIENT);
	if (IS_ERR(qdss))
		return ERR_CAST(qdss);
	ch = &qdss->ch;
	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	if (qdss->usb_connected)
		queue_work(qdss->wq, &qdss->connect_w);
	spin_unlock_irqrestore(&qdss_lock, flags);
	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	qdss = ch->priv_usb;
	if (!qdss || !qdss->client_owned) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		qdss_put_owner(qdss, QDSS_CLIENT);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
	qdss_put_owner(qdss, QDSS_CLIENT);
}
EXPORT_SYMBOL(usb_qdss_close);

static void qdss_cleanup(void)
{
	unsigned long flags;

	/* Referenced channels must not be forcibly freed during unregister. */
	spin_lock_irqsave(&qdss_lock, flags);
	WARN_ON(!list_empty(&usb_qdss_ch_list));
	spin_unlock_irqrestore(&qdss_lock, flags);
}

static void qdss_free_func(struct usb_function *f)
{
	qdss_put_owner(func_to_qdss(f), QDSS_FUNCTION);
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	bool enabled;

	if (!qdss)
		return -ENODEV;
	spin_lock_irqsave(&qdss_lock, flags);
	enabled = qdss->debug_inface_enabled;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return snprintf(page, PAGE_SIZE, "%s\n", enabled ? "Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (!qdss)
		return -ENODEV;
	if (!page || kstrtou8(page, 0, &stats) || stats > 1)
		return -EINVAL;
	spin_lock_irqsave(&qdss_lock, flags);
	if (qdss->function_allocated || qdss->client_owned || qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return -EBUSY;
	}
	qdss->debug_inface_enabled = stats == 1;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	if (opts->usb_qdss)
		qdss_put_owner(opts->usb_qdss, QDSS_INSTANCE);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	struct f_qdss *qdss;

	if (opts->usb_qdss)
		return -EBUSY;
	if (strlen(name) + 1 > 15)
		return -ENAMETOOLONG;
	qdss = qdss_get_owner(name, QDSS_INSTANCE);
	if (IS_ERR(qdss))
		return PTR_ERR(qdss);
	opts->channel_name = (char *)qdss->ch.name;
	opts->usb_qdss = qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;
	unsigned long flags;

	if (!usb_qdss)
		return ERR_PTR(-ENODEV);
	spin_lock_irqsave(&qdss_lock, flags);
	if (usb_qdss->function_allocated) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return ERR_PTR(-EBUSY);
	}
	usb_qdss->function_allocated = true;
	usb_qdss->owners++;
	atomic_inc(&c5_qdss_live_functions);
	spin_unlock_irqrestore(&qdss_lock, flags);

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#else
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	struct usb_request *req;
	struct list_head *act, *tmp;

	pr_debug("usb_qdss_free_req\n");

	qdss = ch->priv_usb;
	if (!qdss) {
		pr_err("usb_qdss_free_req: qdss ctx is NULL\n");
		return;
	}

	list_for_each_safe(act, tmp, &qdss->data_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.data, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_in, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_read_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_out, req);
	}
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf,
	int no_read_buf)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_request *req;
	struct usb_ep *in;
	struct list_head *list_pool;
	int i;

	pr_debug("usb_qdss_alloc_req\n");

	if (!qdss) {
		pr_err("usb_qdss_alloc_req: channel %s closed\n", ch->name);
		return -ENODEV;
	}

	if ((qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf <= 0)) ||
		(!qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf))) {
		pr_err("usb_qdss_alloc_req: missing params\n");
		return -ENODEV;
	}

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
	}

	for (i = 0; i < no_write_buf; i++) {
		req = usb_ep_alloc_request(in, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req: ctrl_in allocation err\n");
			goto fail;
		}
		req->complete = qdss_write_complete;
		list_add_tail(&req->list, list_pool);
	}

	for (i = 0; i < no_read_buf; i++) {
		req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req:ctrl_out allocation err\n");
			goto fail;
		}
		req->complete = qdss_ctrl_read_complete;
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
	}

	return 0;

fail:
	usb_qdss_free_req(ch);
	return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
	struct f_qdss *qdss = func_to_qdss(f);

	pr_debug("clear_eps\n");

	if (qdss->port.ctrl_in)
		qdss->port.ctrl_in->driver_data = NULL;
	if (qdss->port.ctrl_out)
		qdss->port.ctrl_out->driver_data = NULL;
	if (qdss->port.data)
		qdss->port.data->driver_data = NULL;
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
	pr_debug("clear_desc\n");

	if (gadget_is_superspeed(gadget) && f->ss_descriptors)
		usb_free_descriptors(f->ss_descriptors);

	if (gadget_is_dualspeed(gadget) && f->hs_descriptors)
		usb_free_descriptors(f->hs_descriptors);

	if (f->fs_descriptors)
		usb_free_descriptors(f->fs_descriptors);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget = c->cdev->gadget;
	struct f_qdss *qdss = func_to_qdss(f);
	struct usb_ep *ep;
	int iface, id;

	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		qdss_data_ep_comp_desc.bMaxBurst = 0;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

	/*update fs descriptors*/
	qdss_fs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_fs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_fs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->fs_descriptors = usb_copy_descriptors(qdss_fs_desc);
	} else
		f->fs_descriptors = usb_copy_descriptors(
							qdss_fs_data_only_desc);
	/*update descriptors*/
	qdss_hs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_hs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_hs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->hs_descriptors = usb_copy_descriptors(qdss_hs_desc);
	} else
		f->hs_descriptors = usb_copy_descriptors(
							qdss_hs_data_only_desc);
	if (!f->hs_descriptors) {
		pr_err("usb_copy_descriptors error\n");
		goto fail;
	}

	/* update ss descriptors */
	if (gadget_is_superspeed(gadget)) {
		if (qdss->debug_inface_enabled)
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_desc);
		else
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_data_only_desc);
		if (!f->ss_descriptors) {
			pr_err("usb_copy_descriptors error\n");
			goto fail;
		}
	}

	c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return -ENOTSUPP;
}


static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

	flush_workqueue(qdss->wq);
	c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
				unsigned int alt)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = f->config->cdev->gadget;
	struct usb_qdss_ch *ch = &qdss->ch;
	int ret = 0;

	pr_debug("qdss_set_alt qdss pointer = %pK\n", qdss);
	qdss->gadget = gadget;

	if (alt != 0)
		goto fail1;

	if (gadget->speed != USB_SPEED_SUPER &&
		gadget->speed != USB_SPEED_HIGH) {
		pr_err("qdss_st_alt: qdss supportes HS or SS only\n");
		ret = -EINVAL;
		goto fail1;
	}

	if (intf == qdss->data_iface_id && !qdss->data_enabled) {
		/* Increment usage count on connect */
		usb_gadget_autopm_get_async(qdss->gadget);

		if (config_ep_by_speed(gadget, f, qdss->port.data)) {
			ret = -EINVAL;
			goto fail;
		}

		ret = usb_ep_enable(qdss->port.data);
		if (ret)
			goto fail;

		qdss->port.data->driver_data = qdss;
		qdss->data_enabled = 1;


	} else if ((intf == qdss->ctrl_iface_id) &&
	(qdss->debug_inface_enabled)) {

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_in)) {
			ret = -EINVAL;
			goto fail1;
		}

		ret = usb_ep_enable(qdss->port.ctrl_in);
		if (ret)
			goto fail1;

		qdss->port.ctrl_in->driver_data = qdss;
		qdss->ctrl_in_enabled = 1;

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_out)) {
			ret = -EINVAL;
			goto fail1;
		}


		ret = usb_ep_enable(qdss->port.ctrl_out);
		if (ret)
			goto fail1;

		qdss->port.ctrl_out->driver_data = qdss;
		qdss->ctrl_out_enabled = 1;
	}

	if (qdss->debug_inface_enabled) {
		if (qdss->ctrl_out_enabled && qdss->ctrl_in_enabled &&
			qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF enabled\n");
		}
	} else {
		if (qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF disabled\n");
		}
	}

	if (qdss->usb_connected && ch->app_conn)
		queue_work(qdss->wq, &qdss->connect_w);

	return 0;
fail:
	/* Decrement usage count in case of failure */
	usb_gadget_autopm_put_async(qdss->gadget);
fail1:
	pr_err("qdss_set_alt failed\n");
	qdss_eps_disable(f);
	return ret;
}

static struct f_qdss *alloc_usb_qdss(char *channel_name)
{
	struct f_qdss *qdss;
	int found = 0;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(channel_name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_err("%s: (%s) is already available.\n",
				__func__, channel_name);
		return ERR_PTR(-EEXIST);
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	qdss = kzalloc(sizeof(struct f_qdss), GFP_KERNEL);
	if (!qdss) {
		pr_err("%s: Unable to allocate qdss device\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	qdss->wq = create_singlethread_workqueue(channel_name);
	if (!qdss->wq) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_irqsave(&qdss_lock, flags);
	ch = &qdss->ch;
	ch->name = channel_name;
	list_add_tail(&ch->list, &usb_qdss_ch_list);
	spin_unlock_irqrestore(&qdss_lock, flags);

	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);

	return qdss;
}

int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *priv, unsigned int event,
		struct qdss_request *d_req, struct usb_qdss_ch *))
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;
	unsigned long flags;
	int found = 0;

	pr_debug("usb_qdss_open\n");

	if (!notify) {
		pr_err("usb_qdss_open: notification func is missing\n");
		return NULL;
	}

	spin_lock_irqsave(&qdss_lock, flags);
	/* Check if we already have a channel with this name */
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (!found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_debug("usb_qdss_open failed as %s not found\n", name);
		return NULL;
	}

	pr_debug("usb_qdss_open: qdss ctx found\n");
	qdss = container_of(ch, struct f_qdss, ch);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	spin_unlock_irqrestore(&qdss_lock, flags);

	/* the case USB cabel was connected before qdss called qdss_open */
	if (qdss->usb_connected == 1)
		queue_work(qdss->wq, &qdss->connect_w);

	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_gadget *gadget;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	gadget = qdss->gadget;
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
}
EXPORT_SYMBOL(usb_qdss_close);

static void qdss_cleanup(void)
{
	struct f_qdss *qdss;
	struct list_head *act, *tmp;
	struct usb_qdss_ch *_ch;
	unsigned long flags;

	pr_debug("qdss_cleanup\n");

	list_for_each_safe(act, tmp, &usb_qdss_ch_list) {
		_ch = list_entry(act, struct usb_qdss_ch, list);
		qdss = container_of(_ch, struct f_qdss, ch);
		spin_lock_irqsave(&qdss_lock, flags);
		destroy_workqueue(qdss->wq);
		if (!_ch->priv) {
			list_del(&_ch->list);
			kfree(qdss);
		}
		spin_unlock_irqrestore(&qdss_lock, flags);
	}
}

static void qdss_free_func(struct usb_function *f)
{
	/* Do nothing as usb_qdss_alloc() doesn't alloc anything. */
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	return snprintf(page, PAGE_SIZE, "%s\n",
		(to_f_qdss_opts(item)->usb_qdss->debug_inface_enabled == 1) ?
		"Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (page == NULL) {
		pr_err("Invalid buffer");
		return len;
	}

	if (kstrtou8(page, 0, &stats) != 0 && (stats != 0 || stats != 1)) {
		pr_err("(%u)Wrong value. enter 0 to disable or 1 to enable.\n",
			stats);
		return len;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	qdss->debug_inface_enabled = (stats == 1 ? "true" : "false");
	spin_unlock_irqrestore(&qdss->lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(opts->channel_name, ch->name)) {
			list_del(&ch->list);
			break;
		}
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	kfree(opts->channel_name);
	destroy_workqueue(opts->usb_qdss->wq);
	kfree(opts->usb_qdss);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	char *ptr;
	size_t name_len;
	struct f_qdss *usb_qdss;

	/* get channel_name as expected input qdss.<channel_name> */
	name_len = strlen(name) + 1;
	if (name_len > 15)
		return -ENAMETOOLONG;

	/* get channel name */
	ptr = kstrndup(name, name_len, GFP_KERNEL);
	if (!ptr) {
		pr_err("error:%ld\n", PTR_ERR(ptr));
		return -ENOMEM;
	}

	opts->channel_name = ptr;
	pr_debug("qdss: channel_name:%s\n", opts->channel_name);

	usb_qdss = alloc_usb_qdss(opts->channel_name);
	if (IS_ERR(usb_qdss)) {
		pr_err("Failed to create usb_qdss port(%s)\n",
				opts->channel_name);
		return -ENOMEM;
	}

	opts->usb_qdss = usb_qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#endif

#endif

#else
#ifdef CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC
#ifdef CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static atomic_t c5_qdss_live_contexts = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_functions = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_requests = ATOMIC_INIT(0);

static int c5_qdss_count_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d", atomic_read((const atomic_t *)kp->arg));
}

static const struct kernel_param_ops c5_qdss_count_ops = {
	.get = c5_qdss_count_get,
};
module_param_cb(c5_live_contexts, &c5_qdss_count_ops,
	&c5_qdss_live_contexts, 0400);
module_param_cb(c5_live_functions, &c5_qdss_count_ops,
	&c5_qdss_live_functions, 0400);
module_param_cb(c5_live_requests, &c5_qdss_count_ops, &c5_qdss_live_requests, 0400);


static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

static void qdss_free_pool(struct usb_ep *ep, struct list_head *pool)
{
 struct usb_request *req, *tmp;
 list_for_each_entry_safe(req, tmp, pool, list) {
  list_del(&req->list);
  usb_ep_free_request(ep, req);
  atomic_dec(&c5_qdss_live_requests);
 }
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
 struct f_qdss *qdss;
 unsigned long flags;
 LIST_HEAD(data);
 LIST_HEAD(writes);
 LIST_HEAD(reads);
 if (!ch || !(qdss = ch->priv_usb))
  return;
 spin_lock_irqsave(&qdss->lock, flags);
 list_splice_init(&qdss->data_write_pool, &data);
 list_splice_init(&qdss->ctrl_write_pool, &writes);
 list_splice_init(&qdss->ctrl_read_pool, &reads);
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_free_pool(qdss->port.data, &data);
 qdss_free_pool(qdss->port.ctrl_in, &writes);
 qdss_free_pool(qdss->port.ctrl_out, &reads);
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf, int no_read_buf)
{
 struct f_qdss *qdss;
 struct usb_request *req;
 struct usb_ep *in;
 struct list_head *pool;
 unsigned long flags;
 int i;
 LIST_HEAD(writes);
 LIST_HEAD(reads);
 if (!ch || !(qdss = ch->priv_usb))
  return -ENODEV;
 if (no_write_buf <= 0 || (qdss->debug_inface_enabled ? no_read_buf <= 0 : no_read_buf != 0))
  return -EINVAL;
 in = qdss->debug_inface_enabled ? qdss->port.ctrl_in : qdss->port.data;
 pool = qdss->debug_inface_enabled ? &qdss->ctrl_write_pool : &qdss->data_write_pool;
 if (!in || (no_read_buf && !qdss->port.ctrl_out))
  return -ENODEV;
 for (i = 0; i < no_write_buf; i++) {
  req = usb_ep_alloc_request(in, GFP_ATOMIC);
  if (!req)
   goto fail;
  atomic_inc(&c5_qdss_live_requests);
  req->complete = qdss_write_complete;
  list_add_tail(&req->list, &writes);
 }
 for (i = 0; i < no_read_buf; i++) {
  req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
  if (!req)
   goto fail;
  atomic_inc(&c5_qdss_live_requests);
  req->complete = qdss_ctrl_read_complete;
  list_add_tail(&req->list, &reads);
 }
 spin_lock_irqsave(&qdss->lock, flags);
 list_splice_tail_init(&writes, pool);
 list_splice_tail_init(&reads, &qdss->ctrl_read_pool);
 spin_unlock_irqrestore(&qdss->lock, flags);
 return 0;
fail:
 qdss_free_pool(in, &writes);
 qdss_free_pool(qdss->port.ctrl_out, &reads);
 return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
 struct f_qdss *qdss = func_to_qdss(f);
 if (qdss->port.ctrl_in) {
  usb_ep_autoconfig_release(qdss->port.ctrl_in);
  qdss->port.ctrl_in = NULL;
 }
 if (qdss->port.ctrl_out) {
  usb_ep_autoconfig_release(qdss->port.ctrl_out);
  qdss->port.ctrl_out = NULL;
 }
 if (qdss->port.data) {
  usb_ep_autoconfig_release(qdss->port.data);
  qdss->port.data = NULL;
 }
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
 (void)gadget;
 usb_free_all_descriptors(f);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget;
	struct f_qdss *qdss;
	struct usb_ep *ep;
	int iface, id, ret = -ENODEV;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC) &&
     !IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
		return -EOPNOTSUPP;
	gadget = c->cdev->gadget;
	qdss = func_to_qdss(f);
	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;
 qdss->gadget = gadget;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	qdss_data_ep_comp_desc.bMaxBurst =
  !strcmp(qdss->ch.name, USB_QDSS_CH_MDM) ? 0 : 1;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

 qdss_fs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_hs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_fs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_hs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_fs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 qdss_hs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 ret = usb_assign_descriptors(f,
  qdss->debug_inface_enabled ? qdss_fs_desc : qdss_fs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_hs_desc : qdss_hs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_ss_desc : qdss_ss_data_only_desc,
  NULL);
 if (ret)
  goto fail;

 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return ret;
}


static void qdss_disable(struct usb_function *f);
static void qdss_put_pm(struct f_qdss *qdss);

static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

 qdss_disable(f);
	flush_workqueue(qdss->wq);
 qdss_put_pm(qdss);
 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_put_pm(struct f_qdss *qdss)
{
 unsigned long flags;
 bool held;
 spin_lock_irqsave(&qdss->lock, flags);
 held = qdss->pm_owned;
 qdss->pm_owned = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (held)
  usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

/* Test-client admission is serialized with closing before any work is queued. */
static void c5_qdss_queue_test_work(struct f_qdss *qdss,
                                  struct work_struct *work)
{
 unsigned long flags;
 spin_lock_irqsave(&qdss->lock, flags);
 if (qdss->c5_test_client && qdss->ch.app_conn && !qdss->client_closing)
  queue_work(qdss->wq, work);
 spin_unlock_irqrestore(&qdss->lock, flags);
}

static void c5_qdss_notify_test(struct f_qdss *qdss, unsigned int event)
{
 void (*notify)(void *, unsigned int, struct qdss_request *,
                struct usb_qdss_ch *);
 void *priv;
 unsigned long flags;
 spin_lock_irqsave(&qdss->lock, flags);
 if (!qdss->c5_test_client || !qdss->ch.app_conn || qdss->client_closing) {
  spin_unlock_irqrestore(&qdss->lock, flags);
  return;
 }
 notify = qdss->ch.notify;
 priv = qdss->ch.priv;
 spin_unlock_irqrestore(&qdss->lock, flags);
 /* Only the inert owned callback is admitted; close drains both workers. */
 if (notify)
  notify(priv, event, NULL, event == USB_QDSS_CONNECT ? &qdss->ch : NULL);
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  c5_qdss_notify_test(qdss, USB_QDSS_DISCONNECT);
  return;
 }
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	qdss_put_pm(qdss);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected && !qdss->data_enabled &&
     !qdss->ctrl_in_enabled && !qdss->ctrl_out_enabled && !qdss->pm_owned) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	if (IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC)) {
  if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
   c5_qdss_queue_test_work(qdss, &qdss->disconnect_w);
  qdss_put_pm(qdss);
 } else
  queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  if (READ_ONCE(qdss->usb_connected))
   c5_qdss_notify_test(qdss, USB_QDSS_CONNECT);
  return;
 }

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
 unsigned int alt)
{
 struct f_qdss *qdss = func_to_qdss(f);
 struct usb_gadget *gadget = f->config->cdev->gadget;
 unsigned long flags;
 int ret;
 if (alt || (intf != qdss->data_iface_id &&
     (!qdss->debug_inface_enabled || intf != qdss->ctrl_iface_id)))
  return -EINVAL;
 if (gadget->speed != USB_SPEED_SUPER && gadget->speed != USB_SPEED_HIGH)
  return -EINVAL;
 qdss->gadget = gadget;
 if (intf == qdss->data_iface_id && !qdss->data_enabled) {
  ret = usb_gadget_autopm_get_async(gadget);
  if (ret)
   goto fail;
  spin_lock_irqsave(&qdss->lock, flags);
  qdss->pm_owned = true;
  spin_unlock_irqrestore(&qdss->lock, flags);
  ret = config_ep_by_speed(gadget, f, qdss->port.data);
  if (ret)
   goto fail;
  ret = usb_ep_enable(qdss->port.data);
  if (ret)
   goto fail;
  qdss->port.data->driver_data = qdss;
  qdss->data_enabled = 1;
 }
 if (qdss->debug_inface_enabled && intf == qdss->ctrl_iface_id) {
  if (!qdss->ctrl_in_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_in);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_in);
   if (ret)
    goto fail;
   qdss->port.ctrl_in->driver_data = qdss;
   qdss->ctrl_in_enabled = 1;
  }
  if (!qdss->ctrl_out_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_out);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_out);
   if (ret)
    goto fail;
   qdss->port.ctrl_out->driver_data = qdss;
   qdss->ctrl_out_enabled = 1;
  }
 }
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = qdss->data_enabled &&
  (!qdss->debug_inface_enabled ||
   (qdss->ctrl_in_enabled && qdss->ctrl_out_enabled));
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC)) {
  if (READ_ONCE(qdss->usb_connected))
   c5_qdss_queue_test_work(qdss, &qdss->connect_w);
 } else if (qdss->usb_connected && qdss->ch.app_conn)
  queue_work(qdss->wq, &qdss->connect_w);
 return 0;
fail:
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_eps_disable(f);
 qdss_put_pm(qdss);
 return ret;
}


enum qdss_owner { QDSS_INSTANCE, QDSS_CLIENT, QDSS_FUNCTION };

static bool *qdss_owner_flag(struct f_qdss *qdss, enum qdss_owner owner)
{
	if (owner == QDSS_INSTANCE)
		return &qdss->instance_owned;
	if (owner == QDSS_CLIENT)
		return &qdss->client_owned;
	return &qdss->function_allocated;
}

static void qdss_release_context(struct work_struct *work)
{
	struct f_qdss *qdss = container_of(work, struct f_qdss, release_w);

	/* Never drain an owned workqueue from its own callback or a spinlock. */
	destroy_workqueue(qdss->wq);
	atomic_dec(&c5_qdss_live_contexts);
	kfree(qdss->ch.name);
	kfree(qdss);
}

static void qdss_put_owner(struct f_qdss *qdss, enum qdss_owner owner)
{
	unsigned long flags;
	bool release = false;

	spin_lock_irqsave(&qdss_lock, flags);
	if (WARN_ON(!*qdss_owner_flag(qdss, owner) || !qdss->owners)) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
	*qdss_owner_flag(qdss, owner) = false;
	if (owner == QDSS_FUNCTION)
		atomic_dec(&c5_qdss_live_functions);
	if (!--qdss->owners) {
		list_del(&qdss->ch.list);
		release = true;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (release)
		schedule_work(&qdss->release_w);
}

/* Caller holds qdss_lock. */
static struct f_qdss *qdss_find_owner(const char *name, enum qdss_owner owner)
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;

	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (strcmp(name, ch->name))
			continue;
		qdss = container_of(ch, struct f_qdss, ch);
		if (*qdss_owner_flag(qdss, owner))
			return ERR_PTR(-EEXIST);
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners++;
		return qdss;
	}
	return ERR_PTR(-ENOENT);
}

static struct f_qdss *qdss_get_owner(const char *name, enum qdss_owner owner)
{
	struct f_qdss *qdss, *found;
	unsigned long flags;

	if (!name || !*name)
		return ERR_PTR(-EINVAL);
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (PTR_ERR(found) != -ENOENT)
		return found;

	qdss = kzalloc(sizeof(*qdss), GFP_KERNEL);
	if (!qdss)
		return ERR_PTR(-ENOMEM);
	qdss->ch.name = kstrdup(name, GFP_KERNEL);
	if (!qdss->ch.name) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	qdss->wq = create_singlethread_workqueue(name);
	if (!qdss->wq) {
		kfree(qdss->ch.name);
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);
	INIT_WORK(&qdss->release_w, qdss_release_context);

	/* Recheck after sleeping allocation; exactly one unique name is published. */
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	if (PTR_ERR(found) == -ENOENT) {
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners = 1;
		atomic_inc(&c5_qdss_live_contexts);
		list_add_tail(&qdss->ch.list, &usb_qdss_ch_list);
		spin_unlock_irqrestore(&qdss_lock, flags);
		return qdss;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	destroy_workqueue(qdss->wq);
	kfree(qdss->ch.name);
	kfree(qdss);
	return found;
}


int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
  return -EOPNOTSUPP;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
  return -EOPNOTSUPP;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC))
  return -EOPNOTSUPP;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *, unsigned int, struct qdss_request *,
		struct usb_qdss_ch *))
{
	struct f_qdss *qdss;
	struct usb_qdss_ch *ch;
	unsigned long flags;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC))
		return ERR_PTR(-EOPNOTSUPP);
	if (!notify)
		return NULL;
	qdss = qdss_get_owner(name, QDSS_CLIENT);
	if (IS_ERR(qdss))
		return ERR_CAST(qdss);
	ch = &qdss->ch;
	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	if (qdss->usb_connected)
		queue_work(qdss->wq, &qdss->connect_w);
	spin_unlock_irqrestore(&qdss_lock, flags);
	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

static void c5_qdss_close_test_client(struct f_qdss *qdss)
{
 unsigned long flags;
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->client_closing = true;
 qdss->ch.app_conn = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 cancel_work_sync(&qdss->connect_w);
 cancel_work_sync(&qdss->disconnect_w);
 usb_qdss_free_req(&qdss->ch);
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->ch.priv_usb = NULL;
 qdss->ch.notify = NULL;
 qdss->ch.priv = NULL;
 qdss->c5_test_client = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_put_owner(qdss, QDSS_CLIENT);
}

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	qdss = ch->priv_usb;
	if (!qdss || !qdss->client_owned) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
 if (IS_ENABLED(CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC) && qdss->c5_test_client) {
  spin_unlock_irqrestore(&qdss_lock, flags);
  c5_qdss_close_test_client(qdss);
  return;
 }
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		qdss_put_owner(qdss, QDSS_CLIENT);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
	qdss_put_owner(qdss, QDSS_CLIENT);
}
EXPORT_SYMBOL(usb_qdss_close);

#ifdef CONFIG_C5_QDSS_SOFTWARE_CLIENT_DIAGNOSTIC
static DEFINE_MUTEX(c5_qdss_test_lock);
static struct usb_qdss_ch *c5_qdss_test_ch;
static atomic_t c5_test_connect = ATOMIC_INIT(0);
static atomic_t c5_test_disconnect = ATOMIC_INIT(0);
static unsigned int c5_test_opens, c5_test_closes, c5_test_duplicates;
static unsigned int c5_test_allocated, c5_test_freed, c5_test_requests;

static void c5_qdss_test_notify(void *priv, unsigned int event,
 struct qdss_request *req, struct usb_qdss_ch *ch)
{
 if (event == USB_QDSS_CONNECT)
  atomic_inc(&c5_test_connect);
 else if (event == USB_QDSS_DISCONNECT)
  atomic_inc(&c5_test_disconnect);
}

static int c5_qdss_client_probe_set(const char *val, const struct kernel_param *kp)
{
 struct f_qdss *q;
 unsigned long flags;
 unsigned int command, count;
 int ret;
 ret = kstrtouint(val, 0, &command);
 if (ret)
  return ret;
 if (command < 1 || command > 5)
  return -EINVAL;
 mutex_lock(&c5_qdss_test_lock);
 ret = 0;
 if (command == 1) {
  if (c5_qdss_test_ch) {
   ret = -EBUSY;
   goto out;
  }
  q = qdss_get_owner(USB_QDSS_CH_MDM, QDSS_CLIENT);
  if (IS_ERR(q)) {
   ret = PTR_ERR(q);
   goto out;
  }
  spin_lock_irqsave(&q->lock, flags);
  q->client_closing = false;
  q->c5_test_client = true;
  q->ch.priv_usb = q;
  q->ch.priv = NULL;
  q->ch.notify = c5_qdss_test_notify;
  q->ch.app_conn = 1;
  c5_qdss_test_ch = &q->ch;
  spin_unlock_irqrestore(&q->lock, flags);
  c5_test_opens++;
  if (READ_ONCE(q->usb_connected))
   c5_qdss_queue_test_work(q, &q->connect_w);
  goto out;
 }
 if (!c5_qdss_test_ch) {
  ret = -ENODEV;
  goto out;
 }
 q = c5_qdss_test_ch->priv_usb;
 if (command == 2) {
  if (c5_test_requests) {
   ret = -EBUSY;
   goto out;
  }
  if (!READ_ONCE(q->usb_connected)) {
   ret = -ENOTCONN;
   goto out;
  }
  count = q->debug_inface_enabled ? 4 : 2;
  ret = usb_qdss_alloc_req(c5_qdss_test_ch, 2, q->debug_inface_enabled ? 2 : 0);
  if (!ret) {
   c5_test_requests = count;
   c5_test_allocated += count;
  }
 } else if (command == 3) {
  usb_qdss_free_req(c5_qdss_test_ch);
  c5_test_freed += c5_test_requests;
  c5_test_requests = 0;
 } else if (command == 4) {
  usb_qdss_close(c5_qdss_test_ch);
  c5_qdss_test_ch = NULL;
  c5_test_freed += c5_test_requests;
  c5_test_requests = 0;
  c5_test_closes++;
 } else {
  q = qdss_get_owner(USB_QDSS_CH_MDM, QDSS_CLIENT);
  if (IS_ERR(q)) {
   ret = PTR_ERR(q) == -EEXIST ? 0 : PTR_ERR(q);
   if (!ret)
    c5_test_duplicates++;
  } else {
   qdss_put_owner(q, QDSS_CLIENT);
   ret = -EIO;
  }
 }
out:
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}

static int c5_qdss_client_probe_get(char *buffer, const struct kernel_param *kp)
{
 int ret;
 mutex_lock(&c5_qdss_test_lock);
 ret = scnprintf(buffer, PAGE_SIZE,
  "open=%u close=%u duplicate=%u connect=%d disconnect=%d allocated=%u freed=%u requests=%u client=%u\n",
  c5_test_opens, c5_test_closes, c5_test_duplicates,
  atomic_read(&c5_test_connect), atomic_read(&c5_test_disconnect),
  c5_test_allocated, c5_test_freed, c5_test_requests, !!c5_qdss_test_ch);
 mutex_unlock(&c5_qdss_test_lock);
 return ret;
}
static const struct kernel_param_ops c5_qdss_client_probe_ops = {
 .set = c5_qdss_client_probe_set,
 .get = c5_qdss_client_probe_get,
};
module_param_cb(c5_client_probe, &c5_qdss_client_probe_ops, NULL, 0600);
#endif


static void qdss_cleanup(void)
{
	unsigned long flags;

	/* Referenced channels must not be forcibly freed during unregister. */
	spin_lock_irqsave(&qdss_lock, flags);
	WARN_ON(!list_empty(&usb_qdss_ch_list));
	spin_unlock_irqrestore(&qdss_lock, flags);
}

static void qdss_free_func(struct usb_function *f)
{
	qdss_put_owner(func_to_qdss(f), QDSS_FUNCTION);
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	bool enabled;

	if (!qdss)
		return -ENODEV;
	spin_lock_irqsave(&qdss_lock, flags);
	enabled = qdss->debug_inface_enabled;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return snprintf(page, PAGE_SIZE, "%s\n", enabled ? "Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (!qdss)
		return -ENODEV;
	if (!page || kstrtou8(page, 0, &stats) || stats > 1)
		return -EINVAL;
	spin_lock_irqsave(&qdss_lock, flags);
	if (qdss->function_allocated || qdss->client_owned || qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return -EBUSY;
	}
	qdss->debug_inface_enabled = stats == 1;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	if (opts->usb_qdss)
		qdss_put_owner(opts->usb_qdss, QDSS_INSTANCE);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	struct f_qdss *qdss;

	if (opts->usb_qdss)
		return -EBUSY;
	if (strlen(name) + 1 > 15)
		return -ENAMETOOLONG;
	qdss = qdss_get_owner(name, QDSS_INSTANCE);
	if (IS_ERR(qdss))
		return PTR_ERR(qdss);
	opts->channel_name = (char *)qdss->ch.name;
	opts->usb_qdss = qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;
	unsigned long flags;

	if (!usb_qdss)
		return ERR_PTR(-ENODEV);
	spin_lock_irqsave(&qdss_lock, flags);
	if (usb_qdss->function_allocated) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return ERR_PTR(-EBUSY);
	}
	usb_qdss->function_allocated = true;
	usb_qdss->owners++;
	atomic_inc(&c5_qdss_live_functions);
	spin_unlock_irqrestore(&qdss_lock, flags);

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#else
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	struct usb_request *req;
	struct list_head *act, *tmp;

	pr_debug("usb_qdss_free_req\n");

	qdss = ch->priv_usb;
	if (!qdss) {
		pr_err("usb_qdss_free_req: qdss ctx is NULL\n");
		return;
	}

	list_for_each_safe(act, tmp, &qdss->data_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.data, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_in, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_read_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_out, req);
	}
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf,
	int no_read_buf)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_request *req;
	struct usb_ep *in;
	struct list_head *list_pool;
	int i;

	pr_debug("usb_qdss_alloc_req\n");

	if (!qdss) {
		pr_err("usb_qdss_alloc_req: channel %s closed\n", ch->name);
		return -ENODEV;
	}

	if ((qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf <= 0)) ||
		(!qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf))) {
		pr_err("usb_qdss_alloc_req: missing params\n");
		return -ENODEV;
	}

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
	}

	for (i = 0; i < no_write_buf; i++) {
		req = usb_ep_alloc_request(in, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req: ctrl_in allocation err\n");
			goto fail;
		}
		req->complete = qdss_write_complete;
		list_add_tail(&req->list, list_pool);
	}

	for (i = 0; i < no_read_buf; i++) {
		req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req:ctrl_out allocation err\n");
			goto fail;
		}
		req->complete = qdss_ctrl_read_complete;
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
	}

	return 0;

fail:
	usb_qdss_free_req(ch);
	return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
	struct f_qdss *qdss = func_to_qdss(f);

	pr_debug("clear_eps\n");

	if (qdss->port.ctrl_in)
		qdss->port.ctrl_in->driver_data = NULL;
	if (qdss->port.ctrl_out)
		qdss->port.ctrl_out->driver_data = NULL;
	if (qdss->port.data)
		qdss->port.data->driver_data = NULL;
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
	pr_debug("clear_desc\n");

	if (gadget_is_superspeed(gadget) && f->ss_descriptors)
		usb_free_descriptors(f->ss_descriptors);

	if (gadget_is_dualspeed(gadget) && f->hs_descriptors)
		usb_free_descriptors(f->hs_descriptors);

	if (f->fs_descriptors)
		usb_free_descriptors(f->fs_descriptors);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget = c->cdev->gadget;
	struct f_qdss *qdss = func_to_qdss(f);
	struct usb_ep *ep;
	int iface, id;

	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		qdss_data_ep_comp_desc.bMaxBurst = 0;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

	/*update fs descriptors*/
	qdss_fs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_fs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_fs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->fs_descriptors = usb_copy_descriptors(qdss_fs_desc);
	} else
		f->fs_descriptors = usb_copy_descriptors(
							qdss_fs_data_only_desc);
	/*update descriptors*/
	qdss_hs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_hs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_hs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->hs_descriptors = usb_copy_descriptors(qdss_hs_desc);
	} else
		f->hs_descriptors = usb_copy_descriptors(
							qdss_hs_data_only_desc);
	if (!f->hs_descriptors) {
		pr_err("usb_copy_descriptors error\n");
		goto fail;
	}

	/* update ss descriptors */
	if (gadget_is_superspeed(gadget)) {
		if (qdss->debug_inface_enabled)
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_desc);
		else
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_data_only_desc);
		if (!f->ss_descriptors) {
			pr_err("usb_copy_descriptors error\n");
			goto fail;
		}
	}

	c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return -ENOTSUPP;
}


static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

	flush_workqueue(qdss->wq);
	c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
				unsigned int alt)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = f->config->cdev->gadget;
	struct usb_qdss_ch *ch = &qdss->ch;
	int ret = 0;

	pr_debug("qdss_set_alt qdss pointer = %pK\n", qdss);
	qdss->gadget = gadget;

	if (alt != 0)
		goto fail1;

	if (gadget->speed != USB_SPEED_SUPER &&
		gadget->speed != USB_SPEED_HIGH) {
		pr_err("qdss_st_alt: qdss supportes HS or SS only\n");
		ret = -EINVAL;
		goto fail1;
	}

	if (intf == qdss->data_iface_id && !qdss->data_enabled) {
		/* Increment usage count on connect */
		usb_gadget_autopm_get_async(qdss->gadget);

		if (config_ep_by_speed(gadget, f, qdss->port.data)) {
			ret = -EINVAL;
			goto fail;
		}

		ret = usb_ep_enable(qdss->port.data);
		if (ret)
			goto fail;

		qdss->port.data->driver_data = qdss;
		qdss->data_enabled = 1;


	} else if ((intf == qdss->ctrl_iface_id) &&
	(qdss->debug_inface_enabled)) {

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_in)) {
			ret = -EINVAL;
			goto fail1;
		}

		ret = usb_ep_enable(qdss->port.ctrl_in);
		if (ret)
			goto fail1;

		qdss->port.ctrl_in->driver_data = qdss;
		qdss->ctrl_in_enabled = 1;

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_out)) {
			ret = -EINVAL;
			goto fail1;
		}


		ret = usb_ep_enable(qdss->port.ctrl_out);
		if (ret)
			goto fail1;

		qdss->port.ctrl_out->driver_data = qdss;
		qdss->ctrl_out_enabled = 1;
	}

	if (qdss->debug_inface_enabled) {
		if (qdss->ctrl_out_enabled && qdss->ctrl_in_enabled &&
			qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF enabled\n");
		}
	} else {
		if (qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF disabled\n");
		}
	}

	if (qdss->usb_connected && ch->app_conn)
		queue_work(qdss->wq, &qdss->connect_w);

	return 0;
fail:
	/* Decrement usage count in case of failure */
	usb_gadget_autopm_put_async(qdss->gadget);
fail1:
	pr_err("qdss_set_alt failed\n");
	qdss_eps_disable(f);
	return ret;
}

static struct f_qdss *alloc_usb_qdss(char *channel_name)
{
	struct f_qdss *qdss;
	int found = 0;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(channel_name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_err("%s: (%s) is already available.\n",
				__func__, channel_name);
		return ERR_PTR(-EEXIST);
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	qdss = kzalloc(sizeof(struct f_qdss), GFP_KERNEL);
	if (!qdss) {
		pr_err("%s: Unable to allocate qdss device\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	qdss->wq = create_singlethread_workqueue(channel_name);
	if (!qdss->wq) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_irqsave(&qdss_lock, flags);
	ch = &qdss->ch;
	ch->name = channel_name;
	list_add_tail(&ch->list, &usb_qdss_ch_list);
	spin_unlock_irqrestore(&qdss_lock, flags);

	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);

	return qdss;
}

int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *priv, unsigned int event,
		struct qdss_request *d_req, struct usb_qdss_ch *))
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;
	unsigned long flags;
	int found = 0;

	pr_debug("usb_qdss_open\n");

	if (!notify) {
		pr_err("usb_qdss_open: notification func is missing\n");
		return NULL;
	}

	spin_lock_irqsave(&qdss_lock, flags);
	/* Check if we already have a channel with this name */
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (!found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_debug("usb_qdss_open failed as %s not found\n", name);
		return NULL;
	}

	pr_debug("usb_qdss_open: qdss ctx found\n");
	qdss = container_of(ch, struct f_qdss, ch);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	spin_unlock_irqrestore(&qdss_lock, flags);

	/* the case USB cabel was connected before qdss called qdss_open */
	if (qdss->usb_connected == 1)
		queue_work(qdss->wq, &qdss->connect_w);

	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_gadget *gadget;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	gadget = qdss->gadget;
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
}
EXPORT_SYMBOL(usb_qdss_close);

static void qdss_cleanup(void)
{
	struct f_qdss *qdss;
	struct list_head *act, *tmp;
	struct usb_qdss_ch *_ch;
	unsigned long flags;

	pr_debug("qdss_cleanup\n");

	list_for_each_safe(act, tmp, &usb_qdss_ch_list) {
		_ch = list_entry(act, struct usb_qdss_ch, list);
		qdss = container_of(_ch, struct f_qdss, ch);
		spin_lock_irqsave(&qdss_lock, flags);
		destroy_workqueue(qdss->wq);
		if (!_ch->priv) {
			list_del(&_ch->list);
			kfree(qdss);
		}
		spin_unlock_irqrestore(&qdss_lock, flags);
	}
}

static void qdss_free_func(struct usb_function *f)
{
	/* Do nothing as usb_qdss_alloc() doesn't alloc anything. */
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	return snprintf(page, PAGE_SIZE, "%s\n",
		(to_f_qdss_opts(item)->usb_qdss->debug_inface_enabled == 1) ?
		"Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (page == NULL) {
		pr_err("Invalid buffer");
		return len;
	}

	if (kstrtou8(page, 0, &stats) != 0 && (stats != 0 || stats != 1)) {
		pr_err("(%u)Wrong value. enter 0 to disable or 1 to enable.\n",
			stats);
		return len;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	qdss->debug_inface_enabled = (stats == 1 ? "true" : "false");
	spin_unlock_irqrestore(&qdss->lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(opts->channel_name, ch->name)) {
			list_del(&ch->list);
			break;
		}
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	kfree(opts->channel_name);
	destroy_workqueue(opts->usb_qdss->wq);
	kfree(opts->usb_qdss);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	char *ptr;
	size_t name_len;
	struct f_qdss *usb_qdss;

	/* get channel_name as expected input qdss.<channel_name> */
	name_len = strlen(name) + 1;
	if (name_len > 15)
		return -ENAMETOOLONG;

	/* get channel name */
	ptr = kstrndup(name, name_len, GFP_KERNEL);
	if (!ptr) {
		pr_err("error:%ld\n", PTR_ERR(ptr));
		return -ENOMEM;
	}

	opts->channel_name = ptr;
	pr_debug("qdss: channel_name:%s\n", opts->channel_name);

	usb_qdss = alloc_usb_qdss(opts->channel_name);
	if (IS_ERR(usb_qdss)) {
		pr_err("Failed to create usb_qdss port(%s)\n",
				opts->channel_name);
		return -ENOMEM;
	}

	opts->usb_qdss = usb_qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#endif

#else
#ifdef CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static atomic_t c5_qdss_live_contexts = ATOMIC_INIT(0);
static atomic_t c5_qdss_live_functions = ATOMIC_INIT(0);

static int c5_qdss_count_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d", atomic_read((const atomic_t *)kp->arg));
}

static const struct kernel_param_ops c5_qdss_count_ops = {
	.get = c5_qdss_count_get,
};
module_param_cb(c5_live_contexts, &c5_qdss_count_ops,
	&c5_qdss_live_contexts, 0400);
module_param_cb(c5_live_functions, &c5_qdss_count_ops,
	&c5_qdss_live_functions, 0400);


static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	struct usb_request *req;
	struct list_head *act, *tmp;

	pr_debug("usb_qdss_free_req\n");

	qdss = ch->priv_usb;
	if (!qdss) {
		pr_err("usb_qdss_free_req: qdss ctx is NULL\n");
		return;
	}

	list_for_each_safe(act, tmp, &qdss->data_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.data, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_in, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_read_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_out, req);
	}
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf,
	int no_read_buf)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_request *req;
	struct usb_ep *in;
	struct list_head *list_pool;
	int i;

	pr_debug("usb_qdss_alloc_req\n");

	if (!qdss) {
		pr_err("usb_qdss_alloc_req: channel %s closed\n", ch->name);
		return -ENODEV;
	}

	if ((qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf <= 0)) ||
		(!qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf))) {
		pr_err("usb_qdss_alloc_req: missing params\n");
		return -ENODEV;
	}

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
	}

	for (i = 0; i < no_write_buf; i++) {
		req = usb_ep_alloc_request(in, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req: ctrl_in allocation err\n");
			goto fail;
		}
		req->complete = qdss_write_complete;
		list_add_tail(&req->list, list_pool);
	}

	for (i = 0; i < no_read_buf; i++) {
		req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req:ctrl_out allocation err\n");
			goto fail;
		}
		req->complete = qdss_ctrl_read_complete;
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
	}

	return 0;

fail:
	usb_qdss_free_req(ch);
	return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
 struct f_qdss *qdss = func_to_qdss(f);
 if (qdss->port.ctrl_in) {
  usb_ep_autoconfig_release(qdss->port.ctrl_in);
  qdss->port.ctrl_in = NULL;
 }
 if (qdss->port.ctrl_out) {
  usb_ep_autoconfig_release(qdss->port.ctrl_out);
  qdss->port.ctrl_out = NULL;
 }
 if (qdss->port.data) {
  usb_ep_autoconfig_release(qdss->port.data);
  qdss->port.data = NULL;
 }
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
 (void)gadget;
 usb_free_all_descriptors(f);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget;
	struct f_qdss *qdss;
	struct usb_ep *ep;
	int iface, id, ret = -ENODEV;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC) &&
     !IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
		return -EOPNOTSUPP;
	gadget = c->cdev->gadget;
	qdss = func_to_qdss(f);
	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;
 qdss->gadget = gadget;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	qdss_data_ep_comp_desc.bMaxBurst =
  !strcmp(qdss->ch.name, USB_QDSS_CH_MDM) ? 0 : 1;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

 qdss_fs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_hs_data_desc.bEndpointAddress = qdss_ss_data_desc.bEndpointAddress;
 qdss_fs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_hs_ctrl_in_desc.bEndpointAddress = qdss_ss_ctrl_in_desc.bEndpointAddress;
 qdss_fs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 qdss_hs_ctrl_out_desc.bEndpointAddress = qdss_ss_ctrl_out_desc.bEndpointAddress;
 ret = usb_assign_descriptors(f,
  qdss->debug_inface_enabled ? qdss_fs_desc : qdss_fs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_hs_desc : qdss_hs_data_only_desc,
  qdss->debug_inface_enabled ? qdss_ss_desc : qdss_ss_data_only_desc,
  NULL);
 if (ret)
  goto fail;

 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return ret;
}


static void qdss_disable(struct usb_function *f);
static void qdss_put_pm(struct f_qdss *qdss);

static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

 qdss_disable(f);
	flush_workqueue(qdss->wq);
 qdss_put_pm(qdss);
 if (!IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_put_pm(struct f_qdss *qdss)
{
 unsigned long flags;
 bool held;
 spin_lock_irqsave(&qdss->lock, flags);
 held = qdss->pm_owned;
 qdss->pm_owned = false;
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (held)
  usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	qdss_put_pm(qdss);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected && !qdss->data_enabled &&
     !qdss->ctrl_in_enabled && !qdss->ctrl_out_enabled && !qdss->pm_owned) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	if (IS_ENABLED(CONFIG_C5_QDSS_BOUND_DIAGNOSTIC))
  qdss_put_pm(qdss);
 else
  queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
 unsigned int alt)
{
 struct f_qdss *qdss = func_to_qdss(f);
 struct usb_gadget *gadget = f->config->cdev->gadget;
 unsigned long flags;
 int ret;
 if (alt || (intf != qdss->data_iface_id &&
     (!qdss->debug_inface_enabled || intf != qdss->ctrl_iface_id)))
  return -EINVAL;
 if (gadget->speed != USB_SPEED_SUPER && gadget->speed != USB_SPEED_HIGH)
  return -EINVAL;
 qdss->gadget = gadget;
 if (intf == qdss->data_iface_id && !qdss->data_enabled) {
  ret = usb_gadget_autopm_get_async(gadget);
  if (ret)
   goto fail;
  spin_lock_irqsave(&qdss->lock, flags);
  qdss->pm_owned = true;
  spin_unlock_irqrestore(&qdss->lock, flags);
  ret = config_ep_by_speed(gadget, f, qdss->port.data);
  if (ret)
   goto fail;
  ret = usb_ep_enable(qdss->port.data);
  if (ret)
   goto fail;
  qdss->port.data->driver_data = qdss;
  qdss->data_enabled = 1;
 }
 if (qdss->debug_inface_enabled && intf == qdss->ctrl_iface_id) {
  if (!qdss->ctrl_in_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_in);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_in);
   if (ret)
    goto fail;
   qdss->port.ctrl_in->driver_data = qdss;
   qdss->ctrl_in_enabled = 1;
  }
  if (!qdss->ctrl_out_enabled) {
   ret = config_ep_by_speed(gadget, f, qdss->port.ctrl_out);
   if (ret)
    goto fail;
   ret = usb_ep_enable(qdss->port.ctrl_out);
   if (ret)
    goto fail;
   qdss->port.ctrl_out->driver_data = qdss;
   qdss->ctrl_out_enabled = 1;
  }
 }
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = qdss->data_enabled &&
  (!qdss->debug_inface_enabled ||
   (qdss->ctrl_in_enabled && qdss->ctrl_out_enabled));
 spin_unlock_irqrestore(&qdss->lock, flags);
 if (qdss->usb_connected && qdss->ch.app_conn)
  queue_work(qdss->wq, &qdss->connect_w);
 return 0;
fail:
 spin_lock_irqsave(&qdss->lock, flags);
 qdss->usb_connected = 0;
 spin_unlock_irqrestore(&qdss->lock, flags);
 qdss_eps_disable(f);
 qdss_put_pm(qdss);
 return ret;
}


enum qdss_owner { QDSS_INSTANCE, QDSS_CLIENT, QDSS_FUNCTION };

static bool *qdss_owner_flag(struct f_qdss *qdss, enum qdss_owner owner)
{
	if (owner == QDSS_INSTANCE)
		return &qdss->instance_owned;
	if (owner == QDSS_CLIENT)
		return &qdss->client_owned;
	return &qdss->function_allocated;
}

static void qdss_release_context(struct work_struct *work)
{
	struct f_qdss *qdss = container_of(work, struct f_qdss, release_w);

	/* Never drain an owned workqueue from its own callback or a spinlock. */
	destroy_workqueue(qdss->wq);
	atomic_dec(&c5_qdss_live_contexts);
	kfree(qdss->ch.name);
	kfree(qdss);
}

static void qdss_put_owner(struct f_qdss *qdss, enum qdss_owner owner)
{
	unsigned long flags;
	bool release = false;

	spin_lock_irqsave(&qdss_lock, flags);
	if (WARN_ON(!*qdss_owner_flag(qdss, owner) || !qdss->owners)) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
	*qdss_owner_flag(qdss, owner) = false;
	if (owner == QDSS_FUNCTION)
		atomic_dec(&c5_qdss_live_functions);
	if (!--qdss->owners) {
		list_del(&qdss->ch.list);
		release = true;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (release)
		schedule_work(&qdss->release_w);
}

/* Caller holds qdss_lock. */
static struct f_qdss *qdss_find_owner(const char *name, enum qdss_owner owner)
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;

	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (strcmp(name, ch->name))
			continue;
		qdss = container_of(ch, struct f_qdss, ch);
		if (*qdss_owner_flag(qdss, owner))
			return ERR_PTR(-EEXIST);
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners++;
		return qdss;
	}
	return ERR_PTR(-ENOENT);
}

static struct f_qdss *qdss_get_owner(const char *name, enum qdss_owner owner)
{
	struct f_qdss *qdss, *found;
	unsigned long flags;

	if (!name || !*name)
		return ERR_PTR(-EINVAL);
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	spin_unlock_irqrestore(&qdss_lock, flags);
	if (PTR_ERR(found) != -ENOENT)
		return found;

	qdss = kzalloc(sizeof(*qdss), GFP_KERNEL);
	if (!qdss)
		return ERR_PTR(-ENOMEM);
	qdss->ch.name = kstrdup(name, GFP_KERNEL);
	if (!qdss->ch.name) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	qdss->wq = create_singlethread_workqueue(name);
	if (!qdss->wq) {
		kfree(qdss->ch.name);
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}
	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);
	INIT_WORK(&qdss->release_w, qdss_release_context);

	/* Recheck after sleeping allocation; exactly one unique name is published. */
	spin_lock_irqsave(&qdss_lock, flags);
	found = qdss_find_owner(name, owner);
	if (PTR_ERR(found) == -ENOENT) {
		*qdss_owner_flag(qdss, owner) = true;
		qdss->owners = 1;
		atomic_inc(&c5_qdss_live_contexts);
		list_add_tail(&qdss->ch.list, &usb_qdss_ch_list);
		spin_unlock_irqrestore(&qdss_lock, flags);
		return qdss;
	}
	spin_unlock_irqrestore(&qdss_lock, flags);
	destroy_workqueue(qdss->wq);
	kfree(qdss->ch.name);
	kfree(qdss);
	return found;
}


int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *, unsigned int, struct qdss_request *,
		struct usb_qdss_ch *))
{
	struct f_qdss *qdss;
	struct usb_qdss_ch *ch;
	unsigned long flags;


	/* Diagnostic image: forbid endpoint activation and trace clients. */
	if (IS_ENABLED(CONFIG_C5_QDSS_UNBOUND_DIAGNOSTIC))
		return ERR_PTR(-EOPNOTSUPP);
	if (!notify)
		return NULL;
	qdss = qdss_get_owner(name, QDSS_CLIENT);
	if (IS_ERR(qdss))
		return ERR_CAST(qdss);
	ch = &qdss->ch;
	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	if (qdss->usb_connected)
		queue_work(qdss->wq, &qdss->connect_w);
	spin_unlock_irqrestore(&qdss_lock, flags);
	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	qdss = ch->priv_usb;
	if (!qdss || !qdss->client_owned) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		qdss_put_owner(qdss, QDSS_CLIENT);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
	qdss_put_owner(qdss, QDSS_CLIENT);
}
EXPORT_SYMBOL(usb_qdss_close);

static void qdss_cleanup(void)
{
	unsigned long flags;

	/* Referenced channels must not be forcibly freed during unregister. */
	spin_lock_irqsave(&qdss_lock, flags);
	WARN_ON(!list_empty(&usb_qdss_ch_list));
	spin_unlock_irqrestore(&qdss_lock, flags);
}

static void qdss_free_func(struct usb_function *f)
{
	qdss_put_owner(func_to_qdss(f), QDSS_FUNCTION);
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	bool enabled;

	if (!qdss)
		return -ENODEV;
	spin_lock_irqsave(&qdss_lock, flags);
	enabled = qdss->debug_inface_enabled;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return snprintf(page, PAGE_SIZE, "%s\n", enabled ? "Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (!qdss)
		return -ENODEV;
	if (!page || kstrtou8(page, 0, &stats) || stats > 1)
		return -EINVAL;
	spin_lock_irqsave(&qdss_lock, flags);
	if (qdss->function_allocated || qdss->client_owned || qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return -EBUSY;
	}
	qdss->debug_inface_enabled = stats == 1;
	spin_unlock_irqrestore(&qdss_lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	if (opts->usb_qdss)
		qdss_put_owner(opts->usb_qdss, QDSS_INSTANCE);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	struct f_qdss *qdss;

	if (opts->usb_qdss)
		return -EBUSY;
	if (strlen(name) + 1 > 15)
		return -ENAMETOOLONG;
	qdss = qdss_get_owner(name, QDSS_INSTANCE);
	if (IS_ERR(qdss))
		return PTR_ERR(qdss);
	opts->channel_name = (char *)qdss->ch.name;
	opts->usb_qdss = qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;
	unsigned long flags;

	if (!usb_qdss)
		return ERR_PTR(-ENODEV);
	spin_lock_irqsave(&qdss_lock, flags);
	if (usb_qdss->function_allocated) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		return ERR_PTR(-EBUSY);
	}
	usb_qdss->function_allocated = true;
	usb_qdss->owners++;
	atomic_inc(&c5_qdss_live_functions);
	spin_unlock_irqrestore(&qdss_lock, flags);

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#else
#line 1 "/kernel/drivers/usb/gadget/function/f_qdss.c"
/*
 * f_qdss.c -- QDSS function Driver
 *
 * Copyright (c) 2012-2018, 2020, The Linux Foundation. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/usb_qdss.h>
#include <linux/usb/cdc.h>

#include "f_qdss.h"

static DEFINE_SPINLOCK(qdss_lock);
static LIST_HEAD(usb_qdss_ch_list);

static struct usb_interface_descriptor qdss_data_intf_desc = {
	.bLength            =	sizeof(qdss_data_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	1,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_data_desc = {
	.bLength              =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType      =	 USB_DT_ENDPOINT,
	.bEndpointAddress     =	 USB_DIR_IN,
	.bmAttributes         =  USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize       =	 cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor qdss_data_ep_comp_desc = {
	.bLength              =	 sizeof(qdss_data_ep_comp_desc),
	.bDescriptorType      =	 USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst            =	 1,
	.bmAttributes         =	 0,
	.wBytesPerInterval    =	 0,
};

static struct usb_interface_descriptor qdss_ctrl_intf_desc = {
	.bLength            =	sizeof(qdss_ctrl_intf_desc),
	.bDescriptorType    =	USB_DT_INTERFACE,
	.bAlternateSetting  =   0,
	.bNumEndpoints      =	2,
	.bInterfaceClass    =	0xff,
	.bInterfaceSubClass =	0xff,
	.bInterfaceProtocol =	0xff,
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_in_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_IN,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor qdss_hs_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor qdss_ss_ctrl_out_desc = {
	.bLength            =	USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	USB_DT_ENDPOINT,
	.bEndpointAddress   =	USB_DIR_OUT,
	.bmAttributes       =	USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	cpu_to_le16(0x400),
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_in_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_in_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

static struct usb_ss_ep_comp_descriptor qdss_ctrl_out_ep_comp_desc = {
	.bLength            =	sizeof(qdss_ctrl_out_ep_comp_desc),
	.bDescriptorType    =	USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst          =	0,
	.bmAttributes       =	0,
	.wBytesPerInterval  =	0,
};

/* Full speed support */
static struct usb_endpoint_descriptor qdss_fs_data_desc = {
	.bLength            =	 USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =	 USB_DT_ENDPOINT,
	.bEndpointAddress   =	 USB_DIR_IN,
	.bmAttributes       =	 USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =	 cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_in_desc  = {
	.bLength            =    USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =    USB_DT_ENDPOINT,
	.bEndpointAddress   =    USB_DIR_IN,
	.bmAttributes       =    USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =    cpu_to_le16(64),
};

static struct usb_endpoint_descriptor qdss_fs_ctrl_out_desc = {
	.bLength            =     USB_DT_ENDPOINT_SIZE,
	.bDescriptorType    =     USB_DT_ENDPOINT,
	.bEndpointAddress   =     USB_DIR_OUT,
	.bmAttributes       =     USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize     =     cpu_to_le16(64),
};

static struct usb_descriptor_header *qdss_fs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_fs_ctrl_out_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_hs_ctrl_out_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_in_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_in_ep_comp_desc,
	(struct usb_descriptor_header *) &qdss_ss_ctrl_out_desc,
	(struct usb_descriptor_header *) &qdss_ctrl_out_ep_comp_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_fs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_fs_data_desc,
	NULL,
};
static struct usb_descriptor_header *qdss_hs_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_hs_data_desc,
	NULL,
};

static struct usb_descriptor_header *qdss_ss_data_only_desc[] = {
	(struct usb_descriptor_header *) &qdss_data_intf_desc,
	(struct usb_descriptor_header *) &qdss_ss_data_desc,
	(struct usb_descriptor_header *) &qdss_data_ep_comp_desc,
	NULL,
};

/* string descriptors: */
#define QDSS_DATA_IDX	0
#define QDSS_CTRL_IDX	1

static struct usb_string qdss_string_defs[] = {
	[QDSS_DATA_IDX].s = "QDSS DATA",
	[QDSS_CTRL_IDX].s = "QDSS CTRL",
	{}, /* end of list */
};

static struct usb_gadget_strings qdss_string_table = {
	.language =		0x0409,
	.strings =		qdss_string_defs,
};

static struct usb_gadget_strings *qdss_strings[] = {
	&qdss_string_table,
	NULL,
};

static inline struct f_qdss *func_to_qdss(struct usb_function *f)
{
	return container_of(f, struct f_qdss, port.function);
}

static
struct usb_qdss_opts *to_fi_usb_qdss_opts(struct usb_function_instance *fi)
{
	return container_of(fi, struct usb_qdss_opts, func_inst);
}
/*----------------------------------------------------------------------*/

static void qdss_write_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	struct usb_ep *in;
	struct list_head *list_pool;
	enum qdss_state state;
	unsigned long flags;

	pr_debug("qdss_ctrl_write_complete\n");

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
		state = USB_QDSS_CTRL_WRITE_DONE;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
		state = USB_QDSS_DATA_WRITE_DONE;
	}

	if (!req->status) {
		/* send zlp */
		if ((req->length >= ep->maxpacket) &&
				((req->length % ep->maxpacket) == 0)) {
			req->length = 0;
			d_req->actual = req->actual;
			d_req->status = req->status;
			if (!usb_ep_queue(in, req, GFP_ATOMIC))
				return;
		}
	}

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, list_pool);
	if (req->length != 0) {
		d_req->actual = req->actual;
		d_req->status = req->status;
	}
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, state, d_req, NULL);
}

static void qdss_ctrl_read_complete(struct usb_ep *ep,
	struct usb_request *req)
{
	struct f_qdss *qdss = ep->driver_data;
	struct qdss_request *d_req = req->context;
	unsigned long flags;

	pr_debug("qdss_ctrl_read_complete\n");

	d_req->actual = req->actual;
	d_req->status = req->status;

	spin_lock_irqsave(&qdss->lock, flags);
	list_add_tail(&req->list, &qdss->ctrl_read_pool);
	spin_unlock_irqrestore(&qdss->lock, flags);

	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CTRL_READ_DONE, d_req,
			NULL);
}

void usb_qdss_free_req(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss;
	struct usb_request *req;
	struct list_head *act, *tmp;

	pr_debug("usb_qdss_free_req\n");

	qdss = ch->priv_usb;
	if (!qdss) {
		pr_err("usb_qdss_free_req: qdss ctx is NULL\n");
		return;
	}

	list_for_each_safe(act, tmp, &qdss->data_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.data, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_write_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_in, req);
	}

	list_for_each_safe(act, tmp, &qdss->ctrl_read_pool) {
		req = list_entry(act, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(qdss->port.ctrl_out, req);
	}
}
EXPORT_SYMBOL(usb_qdss_free_req);

int usb_qdss_alloc_req(struct usb_qdss_ch *ch, int no_write_buf,
	int no_read_buf)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_request *req;
	struct usb_ep *in;
	struct list_head *list_pool;
	int i;

	pr_debug("usb_qdss_alloc_req\n");

	if (!qdss) {
		pr_err("usb_qdss_alloc_req: channel %s closed\n", ch->name);
		return -ENODEV;
	}

	if ((qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf <= 0)) ||
		(!qdss->debug_inface_enabled &&
		(no_write_buf <= 0 || no_read_buf))) {
		pr_err("usb_qdss_alloc_req: missing params\n");
		return -ENODEV;
	}

	if (qdss->debug_inface_enabled) {
		in = qdss->port.ctrl_in;
		list_pool = &qdss->ctrl_write_pool;
	} else {
		in = qdss->port.data;
		list_pool = &qdss->data_write_pool;
	}

	for (i = 0; i < no_write_buf; i++) {
		req = usb_ep_alloc_request(in, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req: ctrl_in allocation err\n");
			goto fail;
		}
		req->complete = qdss_write_complete;
		list_add_tail(&req->list, list_pool);
	}

	for (i = 0; i < no_read_buf; i++) {
		req = usb_ep_alloc_request(qdss->port.ctrl_out, GFP_ATOMIC);
		if (!req) {
			pr_err("usb_qdss_alloc_req:ctrl_out allocation err\n");
			goto fail;
		}
		req->complete = qdss_ctrl_read_complete;
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
	}

	return 0;

fail:
	usb_qdss_free_req(ch);
	return -ENOMEM;
}
EXPORT_SYMBOL(usb_qdss_alloc_req);

static void clear_eps(struct usb_function *f)
{
	struct f_qdss *qdss = func_to_qdss(f);

	pr_debug("clear_eps\n");

	if (qdss->port.ctrl_in)
		qdss->port.ctrl_in->driver_data = NULL;
	if (qdss->port.ctrl_out)
		qdss->port.ctrl_out->driver_data = NULL;
	if (qdss->port.data)
		qdss->port.data->driver_data = NULL;
}

static void clear_desc(struct usb_gadget *gadget, struct usb_function *f)
{
	pr_debug("clear_desc\n");

	if (gadget_is_superspeed(gadget) && f->ss_descriptors)
		usb_free_descriptors(f->ss_descriptors);

	if (gadget_is_dualspeed(gadget) && f->hs_descriptors)
		usb_free_descriptors(f->hs_descriptors);

	if (f->fs_descriptors)
		usb_free_descriptors(f->fs_descriptors);
}

static int qdss_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_gadget *gadget = c->cdev->gadget;
	struct f_qdss *qdss = func_to_qdss(f);
	struct usb_ep *ep;
	int iface, id;

	pr_debug("qdss_bind\n");

	/* Allocate data I/F */
	iface = usb_interface_id(c, f);
	if (iface < 0) {
		pr_err("interface allocation error\n");
		return iface;
	}
	qdss_data_intf_desc.bInterfaceNumber = iface;
	qdss->data_iface_id = iface;

	id = usb_string_id(c->cdev);
	if (id < 0)
		return id;
	qdss_string_defs[QDSS_DATA_IDX].id = id;
	qdss_data_intf_desc.iInterface = id;

	if (qdss->debug_inface_enabled) {
		/* Allocate ctrl I/F */
		iface = usb_interface_id(c, f);
		if (iface < 0) {
			pr_err("interface allocation error\n");
			return iface;
		}
		qdss_ctrl_intf_desc.bInterfaceNumber = iface;
		qdss->ctrl_iface_id = iface;
		id = usb_string_id(c->cdev);
		if (id < 0)
			return id;
		qdss_string_defs[QDSS_CTRL_IDX].id = id;
		qdss_ctrl_intf_desc.iInterface = id;
	}

	/* for non-accelerated path keep tx fifo size 1k */
	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		qdss_data_ep_comp_desc.bMaxBurst = 0;

	ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_data_desc,
		&qdss_data_ep_comp_desc);
	if (!ep) {
		pr_err("ep_autoconfig error\n");
		goto fail;
	}
	qdss->port.data = ep;
	ep->driver_data = qdss;

	if (qdss->debug_inface_enabled) {
		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_in_desc,
			&qdss_ctrl_in_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_in = ep;
		ep->driver_data = qdss;

		ep = usb_ep_autoconfig_ss(gadget, &qdss_ss_ctrl_out_desc,
			&qdss_ctrl_out_ep_comp_desc);
		if (!ep) {
			pr_err("ep_autoconfig error\n");
			goto fail;
		}
		qdss->port.ctrl_out = ep;
		ep->driver_data = qdss;
	}

	/*update fs descriptors*/
	qdss_fs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_fs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_fs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->fs_descriptors = usb_copy_descriptors(qdss_fs_desc);
	} else
		f->fs_descriptors = usb_copy_descriptors(
							qdss_fs_data_only_desc);
	/*update descriptors*/
	qdss_hs_data_desc.bEndpointAddress =
		qdss_ss_data_desc.bEndpointAddress;
	if (qdss->debug_inface_enabled) {
		qdss_hs_ctrl_in_desc.bEndpointAddress =
		qdss_ss_ctrl_in_desc.bEndpointAddress;
		qdss_hs_ctrl_out_desc.bEndpointAddress =
		qdss_ss_ctrl_out_desc.bEndpointAddress;
		f->hs_descriptors = usb_copy_descriptors(qdss_hs_desc);
	} else
		f->hs_descriptors = usb_copy_descriptors(
							qdss_hs_data_only_desc);
	if (!f->hs_descriptors) {
		pr_err("usb_copy_descriptors error\n");
		goto fail;
	}

	/* update ss descriptors */
	if (gadget_is_superspeed(gadget)) {
		if (qdss->debug_inface_enabled)
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_desc);
		else
			f->ss_descriptors =
			usb_copy_descriptors(qdss_ss_data_only_desc);
		if (!f->ss_descriptors) {
			pr_err("usb_copy_descriptors error\n");
			goto fail;
		}
	}

	c->cdev->gadget->bam2bam_func_enabled = true;
	return 0;
fail:
	clear_eps(f);
	clear_desc(gadget, f);
	return -ENOTSUPP;
}


static void qdss_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = c->cdev->gadget;

	pr_debug("qdss_unbind\n");

	flush_workqueue(qdss->wq);
	c->cdev->gadget->bam2bam_func_enabled = false;
	clear_eps(f);
	clear_desc(gadget, f);
}

static void qdss_eps_disable(struct usb_function *f)
{
	struct f_qdss  *qdss = func_to_qdss(f);

	pr_debug("qdss_eps_disable\n");

	if (qdss->ctrl_in_enabled) {
		usb_ep_disable(qdss->port.ctrl_in);
		qdss->ctrl_in_enabled = 0;
	}

	if (qdss->ctrl_out_enabled) {
		usb_ep_disable(qdss->port.ctrl_out);
		qdss->ctrl_out_enabled = 0;
	}

	if (qdss->data_enabled) {
		usb_ep_disable(qdss->port.data);
		qdss->data_enabled = 0;
	}
}

static void usb_qdss_disconnect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, disconnect_w);
	pr_debug("usb_qdss_disconnect_work\n");


	/* Notify qdss to cancel all active transfers */
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv,
			USB_QDSS_DISCONNECT,
			NULL,
			NULL);

	/* Uninitialized init data i.e. ep specific operation */
	if (qdss->ch.app_conn && !strcmp(qdss->ch.name, USB_QDSS_CH_MSM)) {
		status = uninit_data(qdss->port.data);
		if (status)
			pr_err("%s: uninit_data error\n", __func__);

		status = set_qdss_data_connection(qdss, 0);
		if (status)
			pr_err("qdss_disconnect error");

		spin_lock_irqsave(&qdss->lock, flags);
		if (qdss->endless_req) {
			usb_ep_free_request(qdss->port.data,
					qdss->endless_req);
			qdss->endless_req = NULL;
		}
		spin_unlock_irqrestore(&qdss->lock, flags);
	}

	/*
	 * Decrement usage count which was incremented
	 * before calling connect work
	 */
	usb_gadget_autopm_put_async(qdss->gadget);
}

static void qdss_disable(struct usb_function *f)
{
	struct f_qdss	*qdss = func_to_qdss(f);
	unsigned long flags;

	pr_debug("qdss_disable\n");
	spin_lock_irqsave(&qdss->lock, flags);
	if (!qdss->usb_connected) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return;
	}

	qdss->usb_connected = 0;
	spin_unlock_irqrestore(&qdss->lock, flags);
	/*cancell all active xfers*/
	qdss_eps_disable(f);
	queue_work(qdss->wq, &qdss->disconnect_w);
}

static void usb_qdss_connect_work(struct work_struct *work)
{
	struct f_qdss *qdss;
	int status;
	struct usb_request *req = NULL;
	unsigned long flags;

	qdss = container_of(work, struct f_qdss, connect_w);

	/* If cable is already removed, discard connect_work */
	if (qdss->usb_connected == 0) {
		pr_debug("%s: discard connect_work\n", __func__);
		cancel_work_sync(&qdss->disconnect_w);
		return;
	}

	pr_debug("usb_qdss_connect_work\n");

	if (!strcmp(qdss->ch.name, USB_QDSS_CH_MDM))
		goto notify;

	status = set_qdss_data_connection(qdss, 1);
	if (status) {
		pr_err("set_qdss_data_connection error(%d)", status);
		return;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	req = qdss->endless_req;
	spin_unlock_irqrestore(&qdss->lock, flags);
	if (!req)
		return;

	status = usb_ep_queue(qdss->port.data, req, GFP_ATOMIC);
	if (status) {
		pr_err("%s: usb_ep_queue error (%d)\n", __func__, status);
		return;
	}

notify:
	if (qdss->ch.notify)
		qdss->ch.notify(qdss->ch.priv, USB_QDSS_CONNECT,
						NULL, &qdss->ch);
}

static int qdss_set_alt(struct usb_function *f, unsigned int intf,
				unsigned int alt)
{
	struct f_qdss  *qdss = func_to_qdss(f);
	struct usb_gadget *gadget = f->config->cdev->gadget;
	struct usb_qdss_ch *ch = &qdss->ch;
	int ret = 0;

	pr_debug("qdss_set_alt qdss pointer = %pK\n", qdss);
	qdss->gadget = gadget;

	if (alt != 0)
		goto fail1;

	if (gadget->speed != USB_SPEED_SUPER &&
		gadget->speed != USB_SPEED_HIGH) {
		pr_err("qdss_st_alt: qdss supportes HS or SS only\n");
		ret = -EINVAL;
		goto fail1;
	}

	if (intf == qdss->data_iface_id && !qdss->data_enabled) {
		/* Increment usage count on connect */
		usb_gadget_autopm_get_async(qdss->gadget);

		if (config_ep_by_speed(gadget, f, qdss->port.data)) {
			ret = -EINVAL;
			goto fail;
		}

		ret = usb_ep_enable(qdss->port.data);
		if (ret)
			goto fail;

		qdss->port.data->driver_data = qdss;
		qdss->data_enabled = 1;


	} else if ((intf == qdss->ctrl_iface_id) &&
	(qdss->debug_inface_enabled)) {

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_in)) {
			ret = -EINVAL;
			goto fail1;
		}

		ret = usb_ep_enable(qdss->port.ctrl_in);
		if (ret)
			goto fail1;

		qdss->port.ctrl_in->driver_data = qdss;
		qdss->ctrl_in_enabled = 1;

		if (config_ep_by_speed(gadget, f, qdss->port.ctrl_out)) {
			ret = -EINVAL;
			goto fail1;
		}


		ret = usb_ep_enable(qdss->port.ctrl_out);
		if (ret)
			goto fail1;

		qdss->port.ctrl_out->driver_data = qdss;
		qdss->ctrl_out_enabled = 1;
	}

	if (qdss->debug_inface_enabled) {
		if (qdss->ctrl_out_enabled && qdss->ctrl_in_enabled &&
			qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF enabled\n");
		}
	} else {
		if (qdss->data_enabled) {
			qdss->usb_connected = 1;
			pr_debug("qdss_set_alt usb_connected INTF disabled\n");
		}
	}

	if (qdss->usb_connected && ch->app_conn)
		queue_work(qdss->wq, &qdss->connect_w);

	return 0;
fail:
	/* Decrement usage count in case of failure */
	usb_gadget_autopm_put_async(qdss->gadget);
fail1:
	pr_err("qdss_set_alt failed\n");
	qdss_eps_disable(f);
	return ret;
}

static struct f_qdss *alloc_usb_qdss(char *channel_name)
{
	struct f_qdss *qdss;
	int found = 0;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(channel_name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_err("%s: (%s) is already available.\n",
				__func__, channel_name);
		return ERR_PTR(-EEXIST);
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	qdss = kzalloc(sizeof(struct f_qdss), GFP_KERNEL);
	if (!qdss) {
		pr_err("%s: Unable to allocate qdss device\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	qdss->wq = create_singlethread_workqueue(channel_name);
	if (!qdss->wq) {
		kfree(qdss);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_irqsave(&qdss_lock, flags);
	ch = &qdss->ch;
	ch->name = channel_name;
	list_add_tail(&ch->list, &usb_qdss_ch_list);
	spin_unlock_irqrestore(&qdss_lock, flags);

	spin_lock_init(&qdss->lock);
	INIT_LIST_HEAD(&qdss->ctrl_read_pool);
	INIT_LIST_HEAD(&qdss->ctrl_write_pool);
	INIT_LIST_HEAD(&qdss->data_write_pool);
	INIT_WORK(&qdss->connect_w, usb_qdss_connect_work);
	INIT_WORK(&qdss->disconnect_w, usb_qdss_disconnect_work);

	return qdss;
}

int usb_qdss_ctrl_read(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_read\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_read_pool)) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("error: usb_qdss_ctrl_read list is empty\n");
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_read_pool, struct usb_request, list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;

	if (usb_ep_queue(qdss->port.ctrl_out, req, GFP_ATOMIC)) {
		/* If error add the link to linked list again*/
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_read_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_read);

int usb_qdss_ctrl_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->ctrl_write_pool)) {
		pr_err("error: usb_qdss_ctrl_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->ctrl_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.ctrl_in, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->ctrl_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_ctrl_write);

int usb_qdss_write(struct usb_qdss_ch *ch, struct qdss_request *d_req)
{
	struct f_qdss *qdss = ch->priv_usb;
	unsigned long flags;
	struct usb_request *req = NULL;

	pr_debug("usb_qdss_ctrl_write\n");

	if (!qdss)
		return -ENODEV;

	spin_lock_irqsave(&qdss->lock, flags);

	if (qdss->usb_connected == 0) {
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EIO;
	}

	if (list_empty(&qdss->data_write_pool)) {
		pr_err("error: usb_qdss_data_write list is empty\n");
		spin_unlock_irqrestore(&qdss->lock, flags);
		return -EAGAIN;
	}

	req = list_first_entry(&qdss->data_write_pool, struct usb_request,
		list);
	list_del(&req->list);
	spin_unlock_irqrestore(&qdss->lock, flags);

	req->buf = d_req->buf;
	req->length = d_req->length;
	req->context = d_req;
	if (usb_ep_queue(qdss->port.data, req, GFP_ATOMIC)) {
		spin_lock_irqsave(&qdss->lock, flags);
		list_add_tail(&req->list, &qdss->data_write_pool);
		spin_unlock_irqrestore(&qdss->lock, flags);
		pr_err("qdss usb_ep_queue failed\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(usb_qdss_write);

struct usb_qdss_ch *usb_qdss_open(const char *name, void *priv,
	void (*notify)(void *priv, unsigned int event,
		struct qdss_request *d_req, struct usb_qdss_ch *))
{
	struct usb_qdss_ch *ch;
	struct f_qdss *qdss;
	unsigned long flags;
	int found = 0;

	pr_debug("usb_qdss_open\n");

	if (!notify) {
		pr_err("usb_qdss_open: notification func is missing\n");
		return NULL;
	}

	spin_lock_irqsave(&qdss_lock, flags);
	/* Check if we already have a channel with this name */
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(name, ch->name)) {
			found = 1;
			break;
		}
	}

	if (!found) {
		spin_unlock_irqrestore(&qdss_lock, flags);
		pr_debug("usb_qdss_open failed as %s not found\n", name);
		return NULL;
	}

	pr_debug("usb_qdss_open: qdss ctx found\n");
	qdss = container_of(ch, struct f_qdss, ch);
	ch->priv_usb = qdss;
	ch->priv = priv;
	ch->notify = notify;
	ch->app_conn = 1;
	spin_unlock_irqrestore(&qdss_lock, flags);

	/* the case USB cabel was connected before qdss called qdss_open */
	if (qdss->usb_connected == 1)
		queue_work(qdss->wq, &qdss->connect_w);

	return ch;
}
EXPORT_SYMBOL(usb_qdss_open);

void usb_qdss_close(struct usb_qdss_ch *ch)
{
	struct f_qdss *qdss = ch->priv_usb;
	struct usb_gadget *gadget;
	unsigned long flags;
	int status;

	pr_debug("usb_qdss_close\n");

	spin_lock_irqsave(&qdss_lock, flags);
	ch->priv_usb = NULL;
	if (!qdss || !qdss->usb_connected ||
			!strcmp(qdss->ch.name, USB_QDSS_CH_MDM)) {
		ch->app_conn = 0;
		spin_unlock_irqrestore(&qdss_lock, flags);
		return;
	}

	if (qdss->endless_req) {
		usb_ep_dequeue(qdss->port.data, qdss->endless_req);
		usb_ep_free_request(qdss->port.data, qdss->endless_req);
		qdss->endless_req = NULL;
	}
	gadget = qdss->gadget;
	ch->app_conn = 0;
	spin_unlock_irqrestore(&qdss_lock, flags);

	status = uninit_data(qdss->port.data);
	if (status)
		pr_err("%s: uninit_data error\n", __func__);

	status = set_qdss_data_connection(qdss, 0);
	if (status)
		pr_err("%s:qdss_disconnect error\n", __func__);
}
EXPORT_SYMBOL(usb_qdss_close);

static void qdss_cleanup(void)
{
	struct f_qdss *qdss;
	struct list_head *act, *tmp;
	struct usb_qdss_ch *_ch;
	unsigned long flags;

	pr_debug("qdss_cleanup\n");

	list_for_each_safe(act, tmp, &usb_qdss_ch_list) {
		_ch = list_entry(act, struct usb_qdss_ch, list);
		qdss = container_of(_ch, struct f_qdss, ch);
		spin_lock_irqsave(&qdss_lock, flags);
		destroy_workqueue(qdss->wq);
		if (!_ch->priv) {
			list_del(&_ch->list);
			kfree(qdss);
		}
		spin_unlock_irqrestore(&qdss_lock, flags);
	}
}

static void qdss_free_func(struct usb_function *f)
{
	/* Do nothing as usb_qdss_alloc() doesn't alloc anything. */
}

static inline struct usb_qdss_opts *to_f_qdss_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_qdss_opts,
			func_inst.group);
}

static void qdss_attr_release(struct config_item *item)
{
	struct usb_qdss_opts *opts = to_f_qdss_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations qdss_item_ops = {
	.release	= qdss_attr_release,
};

static ssize_t qdss_enable_debug_inface_show(struct config_item *item,
			char *page)
{
	return snprintf(page, PAGE_SIZE, "%s\n",
		(to_f_qdss_opts(item)->usb_qdss->debug_inface_enabled == 1) ?
		"Enabled" : "Disabled");
}

static ssize_t qdss_enable_debug_inface_store(struct config_item *item,
			const char *page, size_t len)
{
	struct f_qdss *qdss = to_f_qdss_opts(item)->usb_qdss;
	unsigned long flags;
	u8 stats;

	if (page == NULL) {
		pr_err("Invalid buffer");
		return len;
	}

	if (kstrtou8(page, 0, &stats) != 0 && (stats != 0 || stats != 1)) {
		pr_err("(%u)Wrong value. enter 0 to disable or 1 to enable.\n",
			stats);
		return len;
	}

	spin_lock_irqsave(&qdss->lock, flags);
	qdss->debug_inface_enabled = (stats == 1 ? "true" : "false");
	spin_unlock_irqrestore(&qdss->lock, flags);
	return len;
}

CONFIGFS_ATTR(qdss_, enable_debug_inface);
static struct configfs_attribute *qdss_attrs[] = {
	&qdss_attr_enable_debug_inface,
	NULL,
};

static struct config_item_type qdss_func_type = {
	.ct_item_ops	= &qdss_item_ops,
	.ct_attrs	= qdss_attrs,
	.ct_owner	= THIS_MODULE,
};

static void usb_qdss_free_inst(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts;
	struct usb_qdss_ch *ch;
	unsigned long flags;

	opts = container_of(fi, struct usb_qdss_opts, func_inst);
	spin_lock_irqsave(&qdss_lock, flags);
	list_for_each_entry(ch, &usb_qdss_ch_list, list) {
		if (!strcmp(opts->channel_name, ch->name)) {
			list_del(&ch->list);
			break;
		}
	}

	spin_unlock_irqrestore(&qdss_lock, flags);
	kfree(opts->channel_name);
	destroy_workqueue(opts->usb_qdss->wq);
	kfree(opts->usb_qdss);
	kfree(opts);
}

static int usb_qdss_set_inst_name(struct usb_function_instance *f,
				const char *name)
{
	struct usb_qdss_opts *opts =
		container_of(f, struct usb_qdss_opts, func_inst);
	char *ptr;
	size_t name_len;
	struct f_qdss *usb_qdss;

	/* get channel_name as expected input qdss.<channel_name> */
	name_len = strlen(name) + 1;
	if (name_len > 15)
		return -ENAMETOOLONG;

	/* get channel name */
	ptr = kstrndup(name, name_len, GFP_KERNEL);
	if (!ptr) {
		pr_err("error:%ld\n", PTR_ERR(ptr));
		return -ENOMEM;
	}

	opts->channel_name = ptr;
	pr_debug("qdss: channel_name:%s\n", opts->channel_name);

	usb_qdss = alloc_usb_qdss(opts->channel_name);
	if (IS_ERR(usb_qdss)) {
		pr_err("Failed to create usb_qdss port(%s)\n",
				opts->channel_name);
		return -ENOMEM;
	}

	opts->usb_qdss = usb_qdss;
	return 0;
}

static struct usb_function_instance *qdss_alloc_inst(void)
{
	struct usb_qdss_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = usb_qdss_free_inst;
	opts->func_inst.set_inst_name = usb_qdss_set_inst_name;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &qdss_func_type);
	return &opts->func_inst;
}

static struct usb_function *qdss_alloc(struct usb_function_instance *fi)
{
	struct usb_qdss_opts *opts = to_fi_usb_qdss_opts(fi);
	struct f_qdss *usb_qdss = opts->usb_qdss;

	usb_qdss->port.function.name = "usb_qdss";
	usb_qdss->port.function.fs_descriptors = qdss_fs_desc;
	usb_qdss->port.function.hs_descriptors = qdss_hs_desc;
	usb_qdss->port.function.strings = qdss_strings;
	usb_qdss->port.function.bind = qdss_bind;
	usb_qdss->port.function.unbind = qdss_unbind;
	usb_qdss->port.function.set_alt = qdss_set_alt;
	usb_qdss->port.function.disable = qdss_disable;
	usb_qdss->port.function.setup = NULL;
	usb_qdss->port.function.free_func = qdss_free_func;

	return &usb_qdss->port.function;
}

DECLARE_USB_FUNCTION(qdss, qdss_alloc_inst, qdss_alloc);
static int __init usb_qdss_init(void)
{
	int ret;

	INIT_LIST_HEAD(&usb_qdss_ch_list);
	ret = usb_function_register(&qdssusb_func);
	if (ret) {
		pr_err("%s: failed to register diag %d\n", __func__, ret);
		return ret;
	}
	return ret;
}

static void __exit usb_qdss_exit(void)
{
	usb_function_unregister(&qdssusb_func);
	qdss_cleanup();
}

module_init(usb_qdss_init);
module_exit(usb_qdss_exit);
MODULE_DESCRIPTION("USB QDSS Function Driver");

#endif

#endif

#endif

#endif
