#ifdef CONFIG_C5_CI13_LIFECYCLE
#line 1 "/kernel/drivers/usb/gadget/configfs.h"
#ifndef USB__GADGET__CONFIGFS__H
#define USB__GADGET__CONFIGFS__H

#include <linux/configfs.h>

#ifdef CONFIG_C5_CI13_LIFECYCLE
struct mutex *gadget_item_mutex(struct config_item *item);
int unregister_gadget_item_locked(struct config_item *item);
int unregister_gadget_item(struct config_item *item);
#else
void unregister_gadget_item(struct config_item *item);
#endif

struct config_group *usb_os_desc_prepare_interf_dir(
		struct config_group *parent,
		int n_interf,
		struct usb_os_desc **desc,
		char **names,
		struct module *owner);

static inline struct usb_os_desc *to_usb_os_desc(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_os_desc, group);
}

#endif /*  USB__GADGET__CONFIGFS__H */

#else
#line 1 "/kernel/drivers/usb/gadget/configfs.h"
#ifndef USB__GADGET__CONFIGFS__H
#define USB__GADGET__CONFIGFS__H

#include <linux/configfs.h>

#ifdef CONFIG_C5_CI13_LIFECYCLE
int unregister_gadget_item(struct config_item *item);
#else
void unregister_gadget_item(struct config_item *item);
#endif

struct config_group *usb_os_desc_prepare_interf_dir(
		struct config_group *parent,
		int n_interf,
		struct usb_os_desc **desc,
		char **names,
		struct module *owner);

static inline struct usb_os_desc *to_usb_os_desc(struct config_item *item)
{
	return container_of(to_config_group(item), struct usb_os_desc, group);
}

#endif /*  USB__GADGET__CONFIGFS__H */

#endif
