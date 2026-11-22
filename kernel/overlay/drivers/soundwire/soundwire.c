/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/of_device.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/pm_runtime.h>
#include <linux/kref.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/soundwire/soundwire.h>

struct boardinfo {
	struct list_head	list;
	struct swr_boardinfo	board_info;
};

static LIST_HEAD(board_list);
static LIST_HEAD(swr_master_list);
static DEFINE_MUTEX(board_lock);
static DEFINE_IDR(master_idr);
static DEFINE_MUTEX(swr_lock);

static struct device_type swr_dev_type;
static bool swr_core_ready;

#if !IS_ENABLED(CONFIG_C5_SOUNDWIRE_CORE)
#error "This isolated core is built only by CONFIG_C5_SOUNDWIRE_CORE"
#endif
#if IS_ENABLED(CONFIG_SOUNDWIRE) || IS_ENABLED(CONFIG_SOUNDWIRE_WCD_CTRL)
#error "C5 isolated SoundWire core must be the only SoundWire provider"
#endif

#define C5_SWR_TIDS 255U /* Old implementation exposes IDs 0..254, not 256. */
struct c5_swr_txn {
	struct swr_params params; /* Public callback argument layout is unchanged. */
	struct kref refs; /* One callback owner and, while published, one slot owner. */
};

static void c5_swr_txn_release(struct kref *ref)
{
	kfree(container_of(ref, struct c5_swr_txn, refs));
}

/* Caller already owns a valid live master/device borrow. Transaction and
 * slave callbacks run without core locks; the legacy get_logical_dev_num
 * callback alone retains mlock and must be finite and not reenter that lock.
 * The controller stops external producers before unregister, which is sleepable
 * and must not be called from its own callback.
 */
static int c5_swr_begin(struct swr_master *master)
{
	int ret = 0;
	mutex_lock(&master->mlock);
	if (!master->c5_registered || master->c5_stopping)
		ret = -ESHUTDOWN;
	else {
		get_device(&master->dev);
		atomic_inc(&master->c5_callbacks);
	}
	mutex_unlock(&master->mlock);
	return ret;
}

static void c5_swr_end(struct swr_master *master)
{
	if (atomic_dec_and_test(&master->c5_callbacks))
		wake_up_all(&master->c5_callback_wait);
	put_device(&master->dev);
}

/* get_device is taken only while the caller already has a valid live pointer;
 * this is an operation pin, not resurrection of an unprotected stale object.
 * Binding and driver-private-data lifetime remain caller serialized.
 */
static int c5_swr_device_begin(struct swr_device *dev, struct swr_master **master)
{
	int ret;
	if (!dev) return -EINVAL;
	get_device(&dev->dev);
	*master = dev->master;
	if (!*master) { put_device(&dev->dev); return -EINVAL; }
	ret = c5_swr_begin(*master);
	if (ret) put_device(&dev->dev);
	return ret;
}

static void c5_swr_device_end(struct swr_device *dev, struct swr_master *master)
{
	c5_swr_end(master);
	put_device(&dev->dev);
}

static int c5_swr_slave_call(struct swr_device *dev, unsigned int operation)
{
	struct swr_master *master;
	struct swr_driver *driver;
	int (*callback)(struct swr_device *) = NULL;
	int ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	driver = to_swr_driver(dev->dev.driver);
	if (!driver) { ret = -EINVAL; goto out; }
	switch (operation) {
	case 0: callback = driver->startup; break;
	case 1: callback = driver->device_up; break;
	case 2: callback = driver->device_down; break;
	case 3: callback = driver->reset_device; break;
	default: ret = -EINVAL; goto out;
	}
	ret = callback ? callback(dev) : (operation == 0 ? 0 : -ENODEV);
out:
	c5_swr_device_end(dev, master);
	return ret;
}



#define SOUNDWIRE_NAME_SIZE	32

static void swr_master_put(struct swr_master *master)
{
	if (master)
		put_device(&master->dev);
}

static struct swr_master *swr_master_get(struct swr_master *master)
{
	if (!master || !get_device(&master->dev))
		return NULL;
	return master;
}

static void swr_dev_release(struct device *dev)
{
	struct swr_device *swr = to_swr_device(dev);
	struct swr_master *master = swr->master;
	if (master) {
		mutex_lock(&master->mlock);
		list_del_init(&swr->dev_list);
		if (swr->c5_counted) {
			WARN_ON(!master->num_dev);
			if (master->num_dev) master->num_dev--;
		}
		mutex_unlock(&master->mlock);
	}
	of_node_put(dev->of_node);
	swr_master_put(master); /* Exactly one manual child->master reference. */
	kfree(swr);
}

/**
 * swr_new_device - instantiate a new soundwire device
 * @master: Controller to which device is connected
 * @info: Describes the soundwire device
 * Context: can sleep
 *
 * Create a soundwire device. Binding is handled through driver model
 * probe/remove methods. A driver may be bound to this device when
 * the function gets returned.
 *
 * Returns a soundwire new device or NULL
 */
struct swr_device *swr_new_device(struct swr_master *master,
				 struct swr_boardinfo const *info)
{
	struct swr_device *swr;
	int ret;
	if (!master || !info || c5_swr_begin(master)) return NULL;
	swr = kzalloc(sizeof(*swr), GFP_KERNEL);
	if (!swr) goto out_end;
	swr->master = swr_master_get(master);
	swr->addr = info->addr;
	strlcpy(swr->name, info->name, sizeof(swr->name));
	INIT_LIST_HEAD(&swr->dev_list);
	swr->dev.type = &swr_dev_type;
	swr->dev.parent = &master->dev;
	swr->dev.bus = &soundwire_type;
	swr->dev.release = swr_dev_release;
	swr->dev.of_node = of_node_get(info->of_node);
	device_initialize(&swr->dev);
	ret = dev_set_name(&swr->dev, "%s.%lx", swr->name, swr->addr);
	if (ret) goto out_put;
	mutex_lock(&master->mlock);
	list_add_tail(&swr->dev_list, &master->devices);
	mutex_unlock(&master->mlock);
	ret = device_add(&swr->dev);
	if (ret) goto out_put;
	c5_swr_end(master);
	return swr;
out_put:
	put_device(&swr->dev); /* Release owns list, OF node and manual master ref. */
out_end:
	c5_swr_end(master);
	return NULL;
}
EXPORT_SYMBOL(swr_new_device);

/**
 * swr_startup_devices - perform additional initialization for child devices
 *
 * @swr_dev: pointer to soundwire slave device
 *
 * Performs any additional initialization needed for a soundwire slave device.
 * This is a optional functionality defined by slave devices.
 * Removes the slave node from the list, in case there is any failure.
 */
int swr_startup_devices(struct swr_device *dev)
{
	return c5_swr_slave_call(dev, 0);
}
EXPORT_SYMBOL(swr_startup_devices);

/**
 * of_register_swr_devices - register child devices on to the soundwire bus
 * @master: pointer to soundwire master device
 *
 * Registers a soundwire device for each child node of master node which has
 * a "swr-devid" property
 *
 */
int of_register_swr_devices(struct swr_master *master)
{
	struct device_node *node;
	if (!master || !master->dev.of_node) return -EINVAL;
	if (c5_swr_begin(master)) return -ESHUTDOWN;
	for_each_available_child_of_node(master->dev.of_node, node) {
		struct swr_boardinfo info = {};
		struct swr_device *swr;
		u64 addr;
		if (of_modalias_node(node, info.name, sizeof(info.name)) < 0 ||
		    of_property_read_u64(node, "reg", &addr)) continue;
		/* The iterator owns node; new_device takes its independent reference. */
		info.addr = addr;
		info.of_node = node;
		swr = swr_new_device(master, &info);
		if (!swr) continue;
		mutex_lock(&master->mlock);
		if (master->num_dev == 255) {
			mutex_unlock(&master->mlock);
			device_unregister(&swr->dev);
			continue;
		}
		swr->c5_counted = true;
		master->num_dev++;
		mutex_unlock(&master->mlock);
	}
	c5_swr_end(master);
	return 0; /* Retain legacy partial-enumeration return semantics. */
}
EXPORT_SYMBOL(of_register_swr_devices);

/**
 * swr_port_response - response from master to free the completed transaction
 * @mstr: pointer to soundwire master device
 * @tid: transaction id that indicates transaction to be freed.
 *
 * Master calls this function to free the compeleted transaction memory
 */
void swr_port_response(struct swr_master *master, u8 tid)
{
	struct swr_params *params = NULL;
	unsigned long flags;
	if (!master) return;
	spin_lock_irqsave(&master->c5_txn_lock, flags);
	if (tid < master->last_tid) {
		params = master->port_txn[tid];
		master->port_txn[tid] = NULL;
	}
	spin_unlock_irqrestore(&master->c5_txn_lock, flags);
	if (params)
		kref_put(&container_of(params, struct c5_swr_txn, params)->refs,
			 c5_swr_txn_release);
}
EXPORT_SYMBOL(swr_port_response);

/**
 * swr_remove_from_group - remove soundwire slave devices from group
 * @dev: pointer to the soundwire slave device
 * dev_num: device number of the soundwire slave device
 *
 * Returns error code for failure and 0 for success
 */
int swr_remove_from_group(struct swr_device *dev, u8 dev_num)
{
	struct swr_master *master;
	int ret;
	bool skip;
	if (!dev) return -ENODEV;
	master = dev->master;
	if (!master) return -EINVAL;
	ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	mutex_lock(&master->mlock);
	skip = !dev->group_id || master->gr_sid == dev_num; /* Exact OLD predicate. */
	mutex_unlock(&master->mlock);
	if (!skip && master->remove_from_group) master->remove_from_group(master);
	c5_swr_device_end(dev, master);
	return 0;
}
EXPORT_SYMBOL(swr_remove_from_group);

/**
 * swr_slvdev_datapath_control - Enables/Disables soundwire slave device
 *                               data path
 * @dev: pointer to soundwire slave device
 * @dev_num: device number of the soundwire slave device
 *
 * Returns error code for failure and 0 for success
 */
int swr_slvdev_datapath_control(struct swr_device *dev, u8 dev_num, bool enable)
{
	struct swr_master *master;
	int ret;
	if (!dev) return -ENODEV;
	master = dev->master;
	if (!master) return -EINVAL;
	ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	mutex_lock(&master->mlock);
	if (dev->group_id && master->gr_sid != dev_num) {
		if (!master->gr_sid) master->gr_sid = dev_num;
		else { mutex_unlock(&master->mlock); goto out; }
	}
	mutex_unlock(&master->mlock);
	if (master->slvdev_datapath_control) master->slvdev_datapath_control(master, enable);
out:
	c5_swr_device_end(dev, master);
	return 0;
}
EXPORT_SYMBOL(swr_slvdev_datapath_control);


static int c5_swr_port(struct swr_device *dev, u8 *port_id, u8 num_port,
		u8 *ch_mask, u32 *ch_rate, u8 *num_ch, bool connect)
{
	struct swr_master *master;
	struct c5_swr_txn *txn;
	unsigned long flags;
	unsigned int i, tid;
	int ret;
	bool remove_slot = false;
	if (!dev || !dev->master) return -EINVAL;
	master = dev->master;
	if (num_port > SWR_MAX_DEV_PORT_NUM ||
	    (num_port && (!port_id || (connect && (!ch_mask || !ch_rate || !num_ch)))))
		return -EINVAL;
	if ((connect && !master->connect_port) || (!connect && !master->disconnect_port))
		return -EOPNOTSUPP;
	ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	txn = kzalloc(sizeof(*txn), GFP_KERNEL);
	if (!txn) { ret = -ENOMEM; goto out_end; }
	kref_init(&txn->refs); /* Callback owner. */
	txn->params.dev_id = dev->dev_num;
	txn->params.num_port = num_port;
	for (i = 0; i < num_port; i++) {
		txn->params.port_id[i] = port_id[i];
		if (connect) {
			txn->params.num_ch[i] = num_ch[i];
			txn->params.ch_rate[i] = ch_rate[i];
			txn->params.ch_en[i] = ch_mask[i];
		}
	}
	spin_lock_irqsave(&master->c5_txn_lock, flags);
	for (tid = 0; tid < master->last_tid; tid++)
		if (!master->port_txn[tid]) break;
	if (tid == master->last_tid) {
		spin_unlock_irqrestore(&master->c5_txn_lock, flags);
		ret = -ENOMEM;
		goto out_txn;
	}
	txn->params.tid = tid;
	kref_get(&txn->refs); /* Published slot owner. */
	master->port_txn[tid] = &txn->params;
	spin_unlock_irqrestore(&master->c5_txn_lock, flags);
	ret = connect ? master->connect_port(master, &txn->params) :
		master->disconnect_port(master, &txn->params);
	/* A synchronous response may already have removed A and reused its TID
	 * for B. A still owns its callback reference; pointer identity, not just
	 * the numeric TID, determines whether this error may remove a slot.
	 */
	spin_lock_irqsave(&master->c5_txn_lock, flags);
	if (ret && master->port_txn[tid] == &txn->params) {
		master->port_txn[tid] = NULL;
		remove_slot = true;
	}
	spin_unlock_irqrestore(&master->c5_txn_lock, flags);
	if (remove_slot) kref_put(&txn->refs, c5_swr_txn_release);
out_txn:
	kref_put(&txn->refs, c5_swr_txn_release);
out_end:
	c5_swr_device_end(dev, master);
	return ret;
}

/**
 * swr_connect_port - enable soundwire slave port(s)
 * @dev: pointer to soundwire slave device
 * @port_id: logical port id(s) of soundwire slave device
 * @num_port: number of slave device ports need to be enabled
 * @ch_mask: channels for each port that needs to be enabled
 * @ch_rate: rate at which each port/channels operate
 * @num_ch: number of channels for each port
 *
 * soundwire slave device call swr_connect_port API to enable all/some of
 * its ports and corresponding channels and channel rate. This API will
 * call master connect_port callback function to calculate frame structure
 * and enable master and slave ports
 */
int swr_connect_port(struct swr_device *dev, u8 *port_id, u8 num_port,
			u8 *ch_mask, u32 *ch_rate, u8 *num_ch)
{
	return c5_swr_port(dev, port_id, num_port, ch_mask, ch_rate, num_ch, true);
}
EXPORT_SYMBOL(swr_connect_port);

/**
 * swr_disconnect_port - disable soundwire slave port(s)
 * @dev: pointer to soundwire slave device
 * @port_id: logical port id(s) of soundwire slave device
 * @num_port: number of slave device ports need to be disabled
 *
 * soundwire slave device call swr_disconnect_port API to disable all/some of
 * its ports. This API will call master disconnect_port callback function to
 * disable master and slave port and (re)configure frame structure
 */
int swr_disconnect_port(struct swr_device *dev, u8 *port_id, u8 num_port)
{
	return c5_swr_port(dev, port_id, num_port, NULL, NULL, NULL, false);
}
EXPORT_SYMBOL(swr_disconnect_port);

/**
 * swr_get_logical_dev_num - Get soundwire slave logical device number
 * @dev: pointer to soundwire slave device
 * @dev_id: physical device id of soundwire slave device
 * @dev_num: pointer to logical device num of soundwire slave device
 *
 * This API will get the logical device number of soundwire slave device
 */
int swr_get_logical_dev_num(struct swr_device *dev, u64 dev_id, u8 *dev_num)
{
	struct swr_master *master;
	int ret;
	if (!dev || !dev->master || !dev_num) return -EINVAL;
	master = dev->master;
	if (!master->get_logical_dev_num) return -EOPNOTSUPP;
	ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	mutex_lock(&master->mlock);
	ret = master->get_logical_dev_num(master, dev_id, dev_num);
	mutex_unlock(&master->mlock);
	c5_swr_device_end(dev, master);
	return ret;
}
EXPORT_SYMBOL(swr_get_logical_dev_num);

/**
 * swr_read - read soundwire slave device registers
 * @dev: pointer to soundwire slave device
 * @dev_num: logical device num of soundwire slave device
 * @reg_addr: base register address that needs to be read
 * @buf: pointer to store the values of registers from base address
 * @len: length of the buffer
 *
 * This API will read the value of the register address from
 * soundwire slave device
 */
int swr_read(struct swr_device *dev, u8 dev_num, u16 reg_addr,
	     void *buf, u32 len)
{
	struct swr_master *master;
	int ret;
	if (!dev || !dev->master) return -EINVAL;
	master = dev->master;
	if (!master->read) return -EOPNOTSUPP;
	ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	ret = master->read(master, dev_num, reg_addr, buf, len);
	c5_swr_device_end(dev, master);
	return ret;
}
EXPORT_SYMBOL(swr_read);

/**
 * swr_bulk_write - write soundwire slave device registers
 * @dev: pointer to soundwire slave device
 * @dev_num: logical device num of soundwire slave device
 * @reg_addr: register address of soundwire slave device
 * @buf: contains value of register address
 * @len: indicates number of registers
 *
 * This API will write the value of the register address to
 * soundwire slave device
 */
int swr_bulk_write(struct swr_device *dev, u8 dev_num, void *reg,
		   const void *buf, size_t len)
{
	struct swr_master *master;
	int ret;
	if (!dev || !dev->master) return -EINVAL;
	master = dev->master;
	ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	mutex_lock(&master->mlock);
	if (dev->group_id) {
		if (master->gr_sid != dev_num) {
			if (!master->gr_sid) master->gr_sid = dev_num;
			else { mutex_unlock(&master->mlock); ret = 0; goto out; }
		}
		dev_num = dev->group_id;
	}
	mutex_unlock(&master->mlock);
	ret = master->bulk_write ? master->bulk_write(master, dev_num, reg, buf, len) : -ENOSYS;
out:
	c5_swr_device_end(dev, master);
	return ret;
}
EXPORT_SYMBOL(swr_bulk_write);

/**
 * swr_write - write soundwire slave device registers
 * @dev: pointer to soundwire slave device
 * @dev_num: logical device num of soundwire slave device
 * @reg_addr: register address of soundwire slave device
 * @buf: contains value of register address
 *
 * This API will write the value of the register address to
 * soundwire slave device
 */
int swr_write(struct swr_device *dev, u8 dev_num, u16 reg_addr, const void *buf)
{
	struct swr_master *master;
	int ret;
	if (!dev || !dev->master) return -EINVAL;
	master = dev->master;
	if (!master->write) return -EOPNOTSUPP;
	ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	mutex_lock(&master->mlock);
	if (dev->group_id) {
		if (master->gr_sid != dev_num) {
			if (!master->gr_sid) master->gr_sid = dev_num;
			else { mutex_unlock(&master->mlock); ret = 0; goto out; }
		}
		dev_num = dev->group_id;
	}
	mutex_unlock(&master->mlock);
	ret = master->write(master, dev_num, reg_addr, buf);
out:
	c5_swr_device_end(dev, master);
	return ret;
}
EXPORT_SYMBOL(swr_write);

/**
 * swr_device_up - Function to bringup the soundwire slave device
 * @swr_dev: pointer to soundwire slave device
 * Context: can sleep
 *
 * This API will be called by soundwire master to bringup the slave
 * device.
 */
int swr_device_up(struct swr_device *dev)
{
	return c5_swr_slave_call(dev, 1);
}
EXPORT_SYMBOL(swr_device_up);

/**
 * swr_device_down - Function to call soundwire slave device down
 * @swr_dev: pointer to soundwire slave device
 * Context: can sleep
 *
 * This API will be called by soundwire master to put slave device in
 * shutdown state.
 */
int swr_device_down(struct swr_device *dev)
{
	return c5_swr_slave_call(dev, 2);
}
EXPORT_SYMBOL(swr_device_down);

/**
 * swr_reset_device - reset soundwire slave device
 * @swr_dev: pointer to soundwire slave device
 * Context: can sleep
 *
 * This API will be called by soundwire master to reset the slave
 * device when the slave device is not responding or in undefined
 * state
 */
int swr_reset_device(struct swr_device *dev)
{
	return c5_swr_slave_call(dev, 3);
}
EXPORT_SYMBOL(swr_reset_device);

/**
 * swr_set_device_group - Assign group id to the slave devices
 * @swr_dev: pointer to soundwire slave device
 * @id: group id to be assigned to slave device
 * Context: can sleep
 *
 * This API will be called either from soundwire master or slave
 * device to assign group id.
 */
int swr_set_device_group(struct swr_device *dev, u8 id)
{
	struct swr_master *master;
	int ret = c5_swr_device_begin(dev, &master);
	if (ret) return ret;
	mutex_lock(&master->mlock);
	dev->group_id = id;
	if (!id) master->gr_sid = 0;
	mutex_unlock(&master->mlock);
	c5_swr_device_end(dev, master);
	return 0;
}
EXPORT_SYMBOL(swr_set_device_group);

static int swr_drv_probe(struct device *dev)
{
	const struct swr_driver *sdrv = to_swr_driver(dev->driver);

	if (!sdrv)
		return -EINVAL;

	if (sdrv->probe)
		return sdrv->probe(to_swr_device(dev));
	return -ENODEV;
}

static int swr_drv_remove(struct device *dev)
{
	const struct swr_driver *sdrv = to_swr_driver(dev->driver);

	if (!sdrv)
		return -EINVAL;

	if (sdrv->remove)
		return sdrv->remove(to_swr_device(dev));
	return -ENODEV;
}

static void swr_drv_shutdown(struct device *dev)
{
	const struct swr_driver *sdrv = to_swr_driver(dev->driver);

	if (!sdrv)
		return;

	if (sdrv->shutdown)
		sdrv->shutdown(to_swr_device(dev));
}

/**
 * swr_driver_register - register a soundwire driver
 * @drv: the driver to register
 * Context: can sleep
 */
int swr_driver_register(struct swr_driver *drv)
{
	bool ready;
	if (!drv) return -EINVAL;
	mutex_lock(&swr_lock); ready = swr_core_ready; mutex_unlock(&swr_lock);
	if (!ready) return -EAGAIN;
	drv->driver.bus = &soundwire_type;
	if (drv->probe)
		drv->driver.probe = swr_drv_probe;
	if (drv->remove)
		drv->driver.remove = swr_drv_remove;

	if (drv->shutdown)
		drv->driver.shutdown = swr_drv_shutdown;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(swr_driver_register);

/**
 * swr_driver_unregister - unregister a soundwire driver
 * @drv: the driver to unregister
 */
void swr_driver_unregister(struct swr_driver *drv)
{
	if (drv)
		driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(swr_driver_unregister);

static void swr_match_ctrl_to_boardinfo(struct swr_master *master,
				struct swr_boardinfo *bi)
{
	struct swr_device *swr;

	if (master->bus_num != bi->bus_num) {
		dev_dbg(&master->dev,
			"%s: master# %d and bi# %d does not match\n",
			__func__, master->bus_num, bi->bus_num);
		return;
	}

	swr = swr_new_device(master, bi);
	if (!swr)
		dev_err(&master->dev, "can't create new device for %s\n",
			bi->name);
}

/**
 * swr_master_add_boarddevices - Add devices registered by board info
 * @master: master to which these devices are to be added to.
 *
 * This API is called by master when it is up and running. If devices
 * on a master were registered before master, this will make sure that
 * they get probed when master is up.
 */
void swr_master_add_boarddevices(struct swr_master *master)
{
	struct boardinfo *bi;
	struct swr_boardinfo *copy = NULL;
	unsigned int count = 0, i = 0;
	if (!master || c5_swr_begin(master)) return;
	mutex_lock(&board_lock);
	list_for_each_entry(bi, &board_list, list)
		if (bi->board_info.bus_num == master->bus_num) count++;
	if (count) {
		copy = kcalloc(count, sizeof(*copy), GFP_KERNEL);
		if (!copy) { mutex_unlock(&board_lock); goto out; }
		list_for_each_entry(bi, &board_list, list)
			if (bi->board_info.bus_num == master->bus_num) copy[i++] = bi->board_info;
	}
	/* No first-call membership is published before snapshot allocation succeeds. */
	if (!master->c5_board_added) {
		list_add_tail(&master->list, &swr_master_list);
		master->c5_board_added = true;
	}
	mutex_unlock(&board_lock);
	/* device_add may bind a slave: never hold board_lock over that callback.
	 * Repeated calls preserve list membership and retry failed child creation;
	 * device-core duplicate-name rejection prevents duplicate live children.
	 */
	for (i = 0; copy && i < count; i++) swr_match_ctrl_to_boardinfo(master, &copy[i]);
	kfree(copy);
out:
	c5_swr_end(master);
}
EXPORT_SYMBOL(swr_master_add_boarddevices);

static void swr_unregister_device(struct swr_device *swr)
{
	if (swr)
		device_unregister(&swr->dev);
}

static void swr_master_release(struct device *dev)
{
	struct swr_master *master = to_swr_master(dev);
	kfree(master->port_txn);
	master->port_txn = NULL;
	of_node_put(dev->of_node);
	dev->of_node = NULL;
	if (master->c5_core_owned)
		kfree(master); /* Successful registration transfers allocation ownership. */
	else
		complete(&master->c5_release_done); /* Failed registration returns it. */
}

#define swr_master_attr_gr NULL
static struct device_type swr_master_type = {
	.groups     = swr_master_attr_gr,
	.release    = swr_master_release,
};

static int __unregister(struct device *dev, void *null)
{
	swr_unregister_device(to_swr_device(dev));
	return 0;
}

/**
 * swr_unregister_master - unregister soundwire master controller
 * @master: the master being unregistered
 *
 * This API is called by master controller driver to unregister
 *  master controller that was registered by swr_register_master API.
 */
void swr_unregister_master(struct swr_master *master)
{
	unsigned int i;
	unsigned long flags;
	if (!master) return;
	mutex_lock(&swr_lock);
	if (idr_find(&master_idr, master->bus_num) != master) {
		mutex_unlock(&swr_lock); return;
	}
	mutex_lock(&master->mlock);
	master->c5_stopping = true;
	master->c5_registered = false;
	mutex_unlock(&master->mlock);
	idr_remove(&master_idr, master->bus_num);
	mutex_unlock(&swr_lock);
	/* Caller first closed external submission admission, drained all in-flight
	 * submitters, then stopped/joined asynchronous response producers. Do not invoke
	 * this API from one of the master's active callbacks (self-deadlock).
	 */
	wait_event(master->c5_callback_wait, atomic_read(&master->c5_callbacks) == 0);
	mutex_lock(&board_lock);
	if (master->c5_board_added) {
		list_del_init(&master->list); master->c5_board_added = false;
	}
	mutex_unlock(&board_lock);
	for (i = 0; i < C5_SWR_TIDS; i++) {
		struct swr_params *params;
		spin_lock_irqsave(&master->c5_txn_lock, flags);
		params = master->port_txn[i]; master->port_txn[i] = NULL;
		spin_unlock_irqrestore(&master->c5_txn_lock, flags);
		if (params) kref_put(&container_of(params, struct c5_swr_txn, params)->refs,
				 c5_swr_txn_release);
	}
	spin_lock_irqsave(&master->c5_txn_lock, flags);
	master->last_tid = 0;
	spin_unlock_irqrestore(&master->c5_txn_lock, flags);
	device_for_each_child(&master->dev, NULL, __unregister);
	device_unregister(&master->dev); /* Final release may wait for child refs. */
}
EXPORT_SYMBOL(swr_unregister_master);

/**
 * swr_register_master - register soundwire master controller
 * @master: master to be registered
 *
 * This API will register master with the framework. master->bus_num
 * is the desired number with which soundwire framework registers the
 * master.
 */
int swr_register_master(struct swr_master *master)
{
	int id, ret;
	if (!master || master->bus_num >= INT_MAX) return -EINVAL;
	mutex_lock(&swr_lock);
	if (!swr_core_ready) { mutex_unlock(&swr_lock); return -EAGAIN; }
	if (idr_find(&master_idr, master->bus_num) == master) {
		mutex_unlock(&swr_lock); return -EBUSY; /* Existing ownership unchanged. */
	}
	id = idr_alloc(&master_idr, master, master->bus_num, master->bus_num + 1, GFP_KERNEL);
	mutex_unlock(&swr_lock);
	if (id < 0) return id; /* Allocation remains owned by the caller. */
	master->bus_num = id;
	mutex_init(&master->mlock);
	INIT_LIST_HEAD(&master->devices); INIT_LIST_HEAD(&master->list);
	spin_lock_init(&master->c5_txn_lock);
	atomic_set(&master->c5_callbacks, 0); init_waitqueue_head(&master->c5_callback_wait);
	init_completion(&master->c5_release_done);
	master->c5_registered = master->c5_stopping = master->c5_board_added = false;
	master->c5_core_owned = false;
	master->last_tid = 0;
	master->port_txn = kcalloc(C5_SWR_TIDS, sizeof(*master->port_txn), GFP_KERNEL);
	if (!master->port_txn) { ret = -ENOMEM; goto out_id; }
	master->last_tid = C5_SWR_TIDS;
	master->dev.bus = &soundwire_type;
	master->dev.type = &swr_master_type;
	master->dev.of_node = of_node_get(master->dev.of_node);
	device_initialize(&master->dev);
	ret = dev_set_name(&master->dev, "swr%u", master->bus_num);
	if (!ret) ret = device_add(&master->dev);
	if (ret) {
		mutex_lock(&swr_lock); idr_remove(&master_idr, master->bus_num); mutex_unlock(&swr_lock);
		put_device(&master->dev);
		/* device release drops core-owned internals; wait before returning the
		 * failed-registration allocation for the caller's one kfree().
		 */
		wait_for_completion(&master->c5_release_done);
		return ret;
	}
	mutex_lock(&master->mlock);
	master->c5_core_owned = master->c5_registered = true;
	mutex_unlock(&master->mlock);
	return 0;
out_id:
	mutex_lock(&swr_lock); idr_remove(&master_idr, master->bus_num); mutex_unlock(&swr_lock);
	return ret;
}
EXPORT_SYMBOL(swr_register_master);

#define swr_device_attr_gr NULL
#define swr_device_uevent NULL
static struct device_type swr_dev_type = {
	.groups    = swr_device_attr_gr,
	.uevent    = swr_device_uevent,
	.release   = swr_dev_release,
};

static const struct swr_device_id *swr_match(const struct swr_device_id *id,
					     const struct swr_device *swr_dev)
{
	while (id->name[0]) {
		if (strcmp(swr_dev->name, id->name) == 0)
			return id;
		id++;
	}
	return NULL;
}

static int swr_device_match(struct device *dev, struct device_driver *driver)
{
	struct swr_device *swr_dev;
	struct swr_driver *drv = to_swr_driver(driver);

	if (!drv)
		return -EINVAL;

	if (dev->type == &swr_dev_type)
		swr_dev = to_swr_device(dev);
	else
		return 0;
	if (drv->id_table)
		return swr_match(drv->id_table, swr_dev) != NULL;

	if (driver->name)
		return strcmp(swr_dev->name, driver->name) == 0;
	return 0;
}
#ifdef CONFIG_PM_SLEEP
static int swr_legacy_suspend(struct device *dev, pm_message_t mesg)
{
	struct swr_device *swr_dev = NULL;
	struct swr_driver *driver;
	if (dev->type == &swr_dev_type)
		swr_dev = to_swr_device(dev);

	if (!swr_dev || !dev->driver)
		return 0;

	driver = to_swr_driver(dev->driver);
	if (!driver->suspend)
		return 0;

	return driver->suspend(swr_dev, mesg);
}

static int swr_legacy_resume(struct device *dev)
{
	struct swr_device *swr_dev = NULL;
	struct swr_driver *driver;
	if (dev->type == &swr_dev_type)
		swr_dev = to_swr_device(dev);

	if (!swr_dev || !dev->driver)
		return 0;

	driver = to_swr_driver(dev->driver);
	if (!driver->resume)
		return 0;

	return driver->resume(swr_dev);
}

static int swr_pm_suspend(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (pm)
		return pm_generic_suspend(dev);
	else
		return swr_legacy_suspend(dev, PMSG_SUSPEND);
}

static int swr_pm_resume(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (pm)
		return pm_generic_resume(dev);
	else
		return swr_legacy_resume(dev);
}
#else
#define swr_pm_suspend	NULL
#define swr_pm_resume	NULL
#endif /*CONFIG_PM_SLEEP*/

static const struct dev_pm_ops soundwire_pm = {
	.suspend = swr_pm_suspend,
	.resume = swr_pm_resume,
	SET_RUNTIME_PM_OPS(
		pm_generic_suspend,
		pm_generic_resume,
		NULL
		)
};

static void c5_swr_root_release(struct device *dev)
{
	/* Static built-in bus root, not dynamically allocated. */
}

struct device soundwire_dev = {
	.init_name = "soundwire",
	.release = c5_swr_root_release,
};

struct bus_type soundwire_type = {
	.name		= "soundwire",
	.match		= swr_device_match,
	.pm		= &soundwire_pm,
};
EXPORT_SYMBOL(soundwire_type);

static void __exit soundwire_exit(void)
{
	mutex_lock(&swr_lock);
	if (WARN_ON(!idr_is_empty(&master_idr))) { mutex_unlock(&swr_lock); return; }
	swr_core_ready = false;
	mutex_unlock(&swr_lock);
	device_unregister(&soundwire_dev);
	bus_unregister(&soundwire_type);
}

static int __init soundwire_init(void)
{
	int ret = bus_register(&soundwire_type);
	if (ret) return ret;
	device_initialize(&soundwire_dev);
	ret = device_add(&soundwire_dev);
	if (ret) {
		put_device(&soundwire_dev);
		bus_unregister(&soundwire_type);
		return ret;
	}
	mutex_lock(&swr_lock); swr_core_ready = true; mutex_unlock(&swr_lock);
	return 0; /* Bus/root only: no master, OF enumeration or hardware callback. */
}
postcore_initcall(soundwire_init);
module_exit(soundwire_exit);


MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("C5 isolated dormant legacy SoundWire core");
MODULE_ALIAS("platform:soundwire");
