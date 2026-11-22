/* Copyright (c) 2010-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/usb/msm_hsusb_hw.h>
#include <linux/usb/ulpi.h>
#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>

#include "ci13xxx_udc.c"

#define MSM_USB_BASE	(udc->regs)

#define CI13XXX_MSM_MAX_LOG2_ITC	7

struct ci13xxx_udc_context {
	int irq;
	void __iomem *regs;
	int wake_gpio;
	int wake_irq;
	bool wake_irq_state;
	struct pinctrl *ci13xxx_pinctrl;
	struct timer_list irq_enable_timer;
	bool irq_disabled;
	bool irq_ready;
	bool retire_powered;
};

static struct ci13xxx_udc_context _udc_ctxt;
#define IRQ_ENABLE_DELAY	(jiffies + msecs_to_jiffies(1000))

/* Called after the powered stop masked CAP_USBINTR, with admission closed.
 * All endpoint timers were initialized before usb_add_gadget_udc publication.
 */
static bool ci13xxx_parked;

static void ci13xxx_msm_drain_idle_sources(struct ci13xxx *udc)
{
	unsigned long flags;
	bool owned_disable;
	int i;

	/* Neither callback can rearm while admission is closed. */
	for (i = 0; i < hw_ep_max; i++)
		del_timer_sync(&udc->ci13xxx_ep[i].prime_timer);
	del_timer_sync(&_udc_ctxt.irq_enable_timer);

	spin_lock_irqsave(&udc_lock, flags);
	owned_disable = _udc_ctxt.irq_disabled;
	_udc_ctxt.irq_disabled = false;
	spin_unlock_irqrestore(&udc_lock, flags);
	/* Balance our depth only; never disable this shared line for removal. */
	if (owned_disable)
		enable_irq(_udc_ctxt.irq);
	if (READ_ONCE(_udc_ctxt.irq_ready))
		synchronize_irq(_udc_ctxt.irq);
}

static int ci13xxx_msm_prepare_retire(struct usb_gadget *gadget)
{
	struct ci13xxx *udc = container_of(gadget, struct ci13xxx, gadget);
	struct device *parent = udc->dma_owner->parent;
	struct msm_otg *motg;
	unsigned long flags;
	int ret;
	bool idle, device_mode = false;
	unsigned int i;
	spin_lock_irqsave(&udc_lock, flags);
	idle = udc->c5_remove_state == C5_REMOVE_PREPARED && !udc->driver;
	udc->c5_remove_state = C5_REMOVE_PREPARING;
	spin_unlock_irqrestore(&udc_lock, flags);
	ci13xxx_msm_drain_idle_sources(udc);
	if (!wait_event_timeout(udc->c5_idle_wait, !READ_ONCE(udc->c5_active_ops),
				msecs_to_jiffies(USB_CTRL_SET_TIMEOUT))) {
		ret = -EBUSY;
		goto finish;
	}
	ci13xxx_msm_drain_idle_sources(udc);
	cancel_delayed_work_sync(&udc->rw_work);
	if (idle) {
		ret = 0;
		goto finish;
	}
	motg = parent ? dev_get_drvdata(parent) : NULL;
	if (!motg || motg->phy.dev != parent || udc->transceiver != &motg->phy ||
	    device_c5_usb_retained(parent)) {
		ret = -ENODEV;
		goto finish;
	}
	if (!_udc_ctxt.retire_powered || atomic_read(&motg->in_lpm) || READ_ONCE(motg->c5_rail_fault)) {
		ret = -EIO;
		goto finish;
	}
	spin_lock_irqsave(&udc_lock, flags);
	device_mode = hw_cread(CAP_USBMODE, USBMODE_CM) == USBMODE_CM_DEVICE;
	if (!device_mode) {
		ret = -EBUSY;
		if (!udc->skip_flush && !udc->vbus_active &&
		    (udc->udc_driver->flags & CI13XXX_PULLUP_ON_VBUS) &&
		    hw_cread(CAP_USBMODE, USBMODE_CM) == USBMODE_CM_HOST) {
			ret = 0;
			for (i = 0; i < udc->storage_eps; i++) {
				struct ci13xxx_ep *ep = &udc->ci13xxx_ep[i];
				if (ep->c5_quiescing || !list_empty(&ep->qh.queue) ||
				    !list_empty(&ep->rw_queue) || ep->last_zptr) {
					ret = -EBUSY;
					break;
				}
			}
		}
	} else {
		/* Stop fetching before flushing every endpoint; never reset an active host. */
		hw_device_state(0);
		ret = 0;
		for (i = 0; i < udc->storage_eps && !ret; i++) {
			struct ci13xxx_ep *ep = &udc->ci13xxx_ep[i];
			ret = hw_ep_flush(ep->num, ep->dir);
		}
		if (!ret && (hw_cread(CAP_ENDPTFLUSH, ~0) || hw_cread(CAP_ENDPTPRIME, ~0) || hw_cread(CAP_ENDPTSTAT, ~0) ||
			     hw_cread(CAP_USBCMD, USBCMD_RS)))
			ret = -EBUSY;
		if (ret)
			ret = hw_device_reset(udc);
		if (!ret && (hw_cread(CAP_ENDPTFLUSH, ~0) || hw_cread(CAP_ENDPTPRIME, ~0) || hw_cread(CAP_ENDPTSTAT, ~0) ||
			     hw_cread(CAP_USBCMD, USBCMD_RS)))
			ret = -EIO;
	}
	spin_unlock_irqrestore(&udc_lock, flags);
	if (!ret)
		ret = ci13xxx_stop_c5_io(gadget, device_mode);
finish:
	spin_lock_irqsave(&udc_lock, flags);
	udc->c5_remove_state = ret ? C5_REMOVE_FAILED : C5_REMOVE_PREPARED;
	if (ret)
		WRITE_ONCE(ci13xxx_parked, true);
	spin_unlock_irqrestore(&udc_lock, flags);
	return ret;
}

static int ci13xxx_msm_retire(struct platform_device *pdev)
{
	struct device *parent = pdev->dev.parent;
	int pm, ret;
#ifdef CONFIG_USB_GADGET_DEBUG_FILES
	dbg_remove_files(&_udc->gadget.dev);
#endif
	pm = pm_runtime_get_sync(parent);
	_udc_ctxt.retire_powered = pm >= 0;
	ret = c5_usb_retire_udc(&_udc->gadget, ci13xxx_msm_prepare_retire);
	if (ret && READ_ONCE(_udc->c5_remove_state) != C5_REMOVE_FAILED) {
		unsigned long flags;
		spin_lock_irqsave(&udc_lock, flags);
		_udc->c5_remove_state = C5_REMOVE_FAILED;
		WRITE_ONCE(ci13xxx_parked, true);
		spin_unlock_irqrestore(&udc_lock, flags);
		ci13xxx_msm_drain_idle_sources(_udc);
	}
	if (pm < 0)
		pm_runtime_put_noidle(parent);
	else if (ret)
		c5_otg_park_usb(&pdev->dev);
	else {
		pm_runtime_mark_last_busy(parent);
		pm_runtime_put_autosuspend(parent);
	}
	return ret;
}

static irqreturn_t msm_udc_irq(int irq, void *data)
{
	struct ci13xxx *udc = _udc;
	irqreturn_t ret;

	if (!udc || ci13xxx_io_get(udc))
		return IRQ_NONE;
	ret = udc_irq();
	ci13xxx_io_put(udc);
	return ret;
}

static void ci13xxx_msm_suspend(void)
{
	struct device *dev = _udc->gadget.dev.parent;

	dev_dbg(dev, "ci13xxx_msm_suspend\n");

	if (_udc_ctxt.wake_irq && !_udc_ctxt.wake_irq_state) {
		enable_irq_wake(_udc_ctxt.wake_irq);
		enable_irq(_udc_ctxt.wake_irq);
		_udc_ctxt.wake_irq_state = true;
	}
}

static void ci13xxx_msm_resume(void)
{
	struct device *dev = _udc->gadget.dev.parent;

	dev_dbg(dev, "ci13xxx_msm_resume\n");

	if (_udc_ctxt.wake_irq && _udc_ctxt.wake_irq_state) {
		disable_irq_wake(_udc_ctxt.wake_irq);
		disable_irq_nosync(_udc_ctxt.wake_irq);
		_udc_ctxt.wake_irq_state = false;
	}
}

static void ci13xxx_msm_disconnect(void)
{
	struct ci13xxx *udc = _udc;
	struct usb_phy *phy = udc->transceiver;

	if (phy && (phy->flags & ENABLE_DP_MANUAL_PULLUP)) {
		usb_phy_io_write(phy,
				 ULPI_MISC_A_VBUSVLDEXT |
				     ULPI_MISC_A_VBUSVLDEXTSEL,
				 ULPI_CLR(ULPI_MISC_A));

		/*
		 * Add memory barrier as it is must to complete
		 * above USB PHY and Link register writes before
		 * moving ahead with USB peripheral mode enumeration,
		 * otherwise USB peripheral mode may not work.
		 */
		mb();
	}
}

/* Link power management will reduce power consumption by
 * short time HW suspend/resume.
 */
static void ci13xxx_msm_set_l1(struct ci13xxx *udc)
{
	int temp;
	struct device *dev = udc->gadget.dev.parent;

	dev_dbg(dev, "Enable link power management\n");

	/* Enable remote wakeup and L1 for IN EPs */
	writel_relaxed(0xffff0000, USB_L1_EP_CTRL);

	temp = readl_relaxed(USB_L1_CONFIG);
	temp |= L1_CONFIG_LPM_EN | L1_CONFIG_REMOTE_WAKEUP |
		L1_CONFIG_GATE_SYS_CLK | L1_CONFIG_PHY_LPM |
		L1_CONFIG_PLL;
	writel_relaxed(temp, USB_L1_CONFIG);
}

static void ci13xxx_msm_connect(void)
{
	struct ci13xxx *udc = _udc;
	struct usb_phy *phy = udc->transceiver;

	if (phy && (phy->flags & ENABLE_DP_MANUAL_PULLUP)) {
		int temp;

		usb_phy_io_write(phy,
				 ULPI_MISC_A_VBUSVLDEXT |
				     ULPI_MISC_A_VBUSVLDEXTSEL,
				 ULPI_SET(ULPI_MISC_A));

		temp = readl_relaxed(USB_GENCONFIG_2);
		temp |= GENCONFIG_2_SESS_VLD_CTRL_EN;
		writel_relaxed(temp, USB_GENCONFIG_2);

		temp = readl_relaxed(USB_USBCMD);
		temp |= USBCMD_SESS_VLD_CTRL;
		writel_relaxed(temp, USB_USBCMD);

		/*
		 * Add memory barrier as it is must to complete
		 * above USB PHY and Link register writes before
		 * moving ahead with USB peripheral mode enumeration,
		 * otherwise USB peripheral mode may not work.
		 */
		mb();
	}
}

static void ci13xxx_msm_reset(void)
{
	struct ci13xxx *udc = _udc;
	struct usb_phy *phy = udc->transceiver;
	struct device *dev = udc->gadget.dev.parent;
	int temp;

	writel_relaxed(0, USB_AHBBURST);
	writel_relaxed(0x08, USB_AHBMODE);

	/* workaround for rx buffer collision issue */
	temp = readl_relaxed(USB_GENCONFIG);
	temp &= ~GENCONFIG_TXFIFO_IDLE_FORCE_DISABLE;
	temp &= ~GENCONFIG_ULPI_SERIAL_EN;
	writel_relaxed(temp, USB_GENCONFIG);

	if (udc->gadget.l1_supported)
		ci13xxx_msm_set_l1(udc);

	if (phy && (phy->flags & ENABLE_SECONDARY_PHY)) {
		int temp;

		dev_dbg(dev, "using secondary hsphy\n");
		temp = readl_relaxed(USB_PHY_CTRL2);
		temp |= (1 << 16);
		writel_relaxed(temp, USB_PHY_CTRL2);

		/*
		 * Add memory barrier to make sure above LINK writes are
		 * complete before moving ahead with USB peripheral mode
		 * enumeration.
		 */
		mb();
	}
}

static void ci13xxx_msm_mark_err_event(void)
{
	struct ci13xxx *udc = _udc;
	struct msm_otg *otg;

	if (udc == NULL)
		return;

	if (udc->transceiver == NULL)
		return;

	otg = container_of(udc->transceiver, struct msm_otg, phy);

	/* This will trigger hardware reset before next connection */
	otg->err_event_seen = true;
}

static void ci13xxx_msm_notify_event(struct ci13xxx *udc, unsigned int event)
{
	struct device *dev = udc->gadget.dev.parent;

	switch (event) {
	case CI13XXX_CONTROLLER_RESET_EVENT:
		dev_info(dev, "CI13XXX_CONTROLLER_RESET_EVENT received\n");
		ci13xxx_msm_reset();
		break;
	case CI13XXX_CONTROLLER_DISCONNECT_EVENT:
		dev_info(dev, "CI13XXX_CONTROLLER_DISCONNECT_EVENT received\n");
		ci13xxx_msm_disconnect();
		ci13xxx_msm_resume();
		break;
	case CI13XXX_CONTROLLER_CONNECT_EVENT:
		dev_info(dev, "CI13XXX_CONTROLLER_CONNECT_EVENT received\n");
		ci13xxx_msm_connect();
		break;
	case CI13XXX_CONTROLLER_SUSPEND_EVENT:
		dev_info(dev, "CI13XXX_CONTROLLER_SUSPEND_EVENT received\n");
		ci13xxx_msm_suspend();
		break;
	case CI13XXX_CONTROLLER_RESUME_EVENT:
		dev_info(dev, "CI13XXX_CONTROLLER_RESUME_EVENT received\n");
		ci13xxx_msm_resume();
		break;
	case CI13XXX_CONTROLLER_ERROR_EVENT:
		dev_info(dev, "CI13XXX_CONTROLLER_ERROR_EVENT received\n");
		ci13xxx_msm_mark_err_event();
		break;
	case CI13XXX_CONTROLLER_UDC_STARTED_EVENT:
		dev_info(dev,
			 "CI13XXX_CONTROLLER_UDC_STARTED_EVENT received\n");
		break;
	default:
		dev_dbg(dev, "unknown ci13xxx_udc event\n");
		break;
	}
}

static bool ci13xxx_msm_in_lpm(struct ci13xxx *udc)
{
	struct msm_otg *otg;

	if (udc == NULL)
		return false;

	if (udc->transceiver == NULL)
		return false;

	otg = container_of(udc->transceiver, struct msm_otg, phy);

	return (atomic_read(&otg->in_lpm) != 0);
}

static irqreturn_t ci13xxx_msm_resume_irq(int irq, void *data)
{
	struct ci13xxx *udc = _udc;

	if (!udc || ci13xxx_io_get(udc))
		return IRQ_NONE;
	if (udc->transceiver && udc->vbus_active && udc->suspended)
		usb_phy_set_suspend(udc->transceiver, 0);
	else if (!udc->suspended)
		ci13xxx_msm_resume();

	ci13xxx_io_put(udc);
	return IRQ_HANDLED;
}

static struct ci13xxx_udc_driver ci13xxx_msm_udc_driver = {
    .name = "ci13xxx_msm",
    .flags = CI13XXX_REGS_SHARED |
	     CI13XXX_REQUIRE_TRANSCEIVER |
	     CI13XXX_PULLUP_ON_VBUS |
	     CI13XXX_ZERO_ITC |
	     CI13XXX_DISABLE_STREAMING,
    .nz_itc = 0,
    .notify_event = ci13xxx_msm_notify_event,
    .in_lpm = ci13xxx_msm_in_lpm,
};

static int ci13xxx_msm_install_wake_gpio(struct platform_device *pdev,
					 struct resource *res)
{
	int wake_irq;
	int ret;
	struct pinctrl_state *set_state;

	dev_dbg(&pdev->dev, "ci13xxx_msm_install_wake_gpio\n");

	_udc_ctxt.wake_gpio = res->start;
	if (_udc_ctxt.ci13xxx_pinctrl) {
		set_state = pinctrl_lookup_state(_udc_ctxt.ci13xxx_pinctrl,
						 "ci13xxx_active");
		if (IS_ERR(set_state)) {
			pr_err("cannot get ci13xxx pinctrl active state\n");
			return PTR_ERR(set_state);
		}
		pinctrl_select_state(_udc_ctxt.ci13xxx_pinctrl, set_state);
	}
	gpio_request(_udc_ctxt.wake_gpio, "USB_RESUME");
	gpio_direction_input(_udc_ctxt.wake_gpio);
	wake_irq = gpio_to_irq(_udc_ctxt.wake_gpio);
	if (wake_irq < 0) {
		dev_err(&pdev->dev, "could not register USB_RESUME GPIO.\n");
		return -ENXIO;
	}

	dev_dbg(&pdev->dev, "_udc_ctxt.gpio_irq = %d and irq = %d\n",
		_udc_ctxt.wake_gpio, wake_irq);
	ret = request_irq(wake_irq, ci13xxx_msm_resume_irq,
			  IRQF_TRIGGER_RISING | IRQF_ONESHOT, "usb resume", NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not register USB_RESUME IRQ.\n");
		goto gpio_free;
	}
	disable_irq(wake_irq);
	_udc_ctxt.wake_irq = wake_irq;

	return 0;

gpio_free:
	gpio_free(_udc_ctxt.wake_gpio);
	if (_udc_ctxt.ci13xxx_pinctrl) {
		set_state = pinctrl_lookup_state(_udc_ctxt.ci13xxx_pinctrl,
						 "ci13xxx_sleep");
		if (IS_ERR(set_state))
			pr_err("cannot get ci13xxx pinctrl sleep state\n");
		else
			pinctrl_select_state(_udc_ctxt.ci13xxx_pinctrl,
					     set_state);
	}
	_udc_ctxt.wake_gpio = 0;
	return ret;
}

static void ci13xxx_msm_uninstall_wake_gpio(struct platform_device *pdev, bool keep_pins)
{
	struct pinctrl_state *set_state;

	dev_dbg(&pdev->dev, "ci13xxx_msm_uninstall_wake_gpio\n");

	if (_udc_ctxt.wake_irq) {
		if (_udc_ctxt.wake_irq_state)
			disable_irq_wake(_udc_ctxt.wake_irq);
		free_irq(_udc_ctxt.wake_irq, NULL);
		_udc_ctxt.wake_irq = 0;
		_udc_ctxt.wake_irq_state = false;
	}
	if (_udc_ctxt.wake_gpio && !keep_pins) {
		gpio_free(_udc_ctxt.wake_gpio);
		if (_udc_ctxt.ci13xxx_pinctrl) {
			set_state =
			    pinctrl_lookup_state(_udc_ctxt.ci13xxx_pinctrl,
						 "ci13xxx_sleep");
			if (IS_ERR(set_state))
				pr_err("cannot get ci13xxx pinctrl sleep state\n");
			else
				pinctrl_select_state(_udc_ctxt.ci13xxx_pinctrl,
						     set_state);
		}
		_udc_ctxt.wake_gpio = 0;
	}
}

static void enable_usb_irq_timer_func(unsigned long data);
static int ci13xxx_msm_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret;
	unsigned long flags;
	struct ci13xxx_platform_data *pdata = pdev->dev.platform_data;
	bool is_l1_supported = false;

	if (READ_ONCE(ci13xxx_parked) || c5_otg_physical_parked(&pdev->dev))
		return -EBUSY;
	c5_ci_probe_begin(&pdev->dev);
	dev_dbg(&pdev->dev, "ci13xxx_msm_probe\n");

	if (pdata) {
		/* Acceptable values for nz_itc are: 0,1,2,4,8,16,32,64 */
		if (pdata->log2_itc > CI13XXX_MSM_MAX_LOG2_ITC ||
		    pdata->log2_itc <= 0)
			ci13xxx_msm_udc_driver.nz_itc = 0;
		else
			ci13xxx_msm_udc_driver.nz_itc =
			    1 << (pdata->log2_itc - 1);

		is_l1_supported = pdata->l1_supported;
		/* Set ahb2ahb bypass flag if it is requested. */
		if (pdata->enable_ahb2ahb_bypass)
			ci13xxx_msm_udc_driver.flags |=
			    CI13XXX_ENABLE_AHB2AHB_BYPASS;

		/* Clear disable streaming flag if is requested. */
		if (pdata->enable_streaming)
			ci13xxx_msm_udc_driver.flags &=
			    ~CI13XXX_DISABLE_STREAMING;
	}

	c5_ci_probe_step(10);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get platform resource mem\n");
		c5_ci_probe_finish(-ENXIO);
		return -ENXIO;
	}

	c5_ci_probe_step(20);
	_udc_ctxt.regs = ioremap(res->start, resource_size(res));
	if (!_udc_ctxt.regs) {
		dev_err(&pdev->dev, "ioremap failed\n");
		c5_ci_probe_finish(-ENOMEM);
		return -ENOMEM;
	}

	c5_ci_probe_step(30);
	setup_timer(&_udc_ctxt.irq_enable_timer, enable_usb_irq_timer_func,
		    (unsigned long)NULL);
	ret = udc_probe(&ci13xxx_msm_udc_driver, &pdev->dev, _udc_ctxt.regs);
	if (ret < 0) {
		dev_err(&pdev->dev, "udc_probe failed: %d\n", ret);
		goto iounmap;
	}

	_udc->gadget.l1_supported = is_l1_supported;

	c5_ci_probe_step(210);
	_udc_ctxt.irq = platform_get_irq(pdev, 0);
	if (_udc_ctxt.irq < 0) {
		dev_err(&pdev->dev, "IRQ not found\n");
		ret = -ENXIO;
		goto udc_remove;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_IO, "USB_RESUME");
	/* Get pinctrl if target uses pinctrl */
	c5_ci_probe_step(220);
	_udc_ctxt.ci13xxx_pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(_udc_ctxt.ci13xxx_pinctrl)) {
		if (of_property_read_bool(pdev->dev.of_node, "pinctrl-names")) {
			dev_err(&pdev->dev, "Error encountered while getting pinctrl");
			ret = PTR_ERR(_udc_ctxt.ci13xxx_pinctrl);
			goto udc_remove;
		}
		dev_dbg(&pdev->dev, "Target does not use pinctrl\n");
		_udc_ctxt.ci13xxx_pinctrl = NULL;
	}
	if (res) {
		c5_ci_probe_step(230);
		ret = ci13xxx_msm_install_wake_gpio(pdev, res);
		if (ret < 0) {
			dev_err(&pdev->dev, "gpio irq install failed\n");
			goto udc_remove;
		}
	}

	c5_ci_probe_step(240);

	ret = request_irq(_udc_ctxt.irq, msm_udc_irq, IRQF_SHARED, pdev->name,
			  pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "request_irq failed\n");
		goto gpio_uninstall;
	}

	c5_ci_probe_step(250);
	spin_lock_irqsave(&udc_lock, flags);
	_udc_ctxt.irq_ready = true;
	spin_unlock_irqrestore(&udc_lock, flags);

	pm_runtime_no_callbacks(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	c5_ci_probe_step(260);
	c5_ci_probe_finish(0);
	return 0;

gpio_uninstall:
	ci13xxx_msm_uninstall_wake_gpio(pdev, false);
udc_remove:
	if (ci13xxx_msm_retire(pdev)) {
		c5_otg_park_usb(&pdev->dev);
		c5_ci_probe_finish(ret);
		return ret;
	}
	udc_release();
iounmap:
	iounmap(_udc_ctxt.regs);

	c5_ci_probe_finish(ret);
	return ret;
}

#ifdef CONFIG_C5_USB_MUIC_ROLE
static int ci13xxx_msm_claim_idle(struct platform_device *pdev)
{
	struct msm_otg *motg = dev_get_drvdata(pdev->dev.parent);
	struct ci13xxx *udc;
	unsigned long flags;
	int i, ret = -EBUSY;

	if (motg->c5_gadget_child != pdev || !spin_trylock_irqsave(&udc_lock, flags))
		return -EBUSY;
	udc = _udc;
	if (!udc || udc->transceiver != &motg->phy || udc->gadget.dev.parent != &pdev->dev || !_udc_ctxt.irq_ready ||
	    _udc_ctxt.wake_irq ||
	    (motg->phy.otg->gadget && motg->phy.otg->gadget != &udc->gadget) ||
	    udc->c5_remove_state != C5_REMOVE_IDLE || udc->c5_active_ops ||
	    udc->driver || udc->vbus_active || udc->configured || udc->suspended ||
	    udc->gadget.speed != USB_SPEED_UNKNOWN || udc->rw_pending ||
	    udc->status || udc->status_buf || udc->skip_flush ||
	    !hw_ep_max || hw_ep_max > ARRAY_SIZE(udc->ci13xxx_ep))
		goto out;
	for (i = 0; i < hw_ep_max; i++) {
		struct ci13xxx_ep *ep = &udc->ci13xxx_ep[i];

		if (ep->c5_quiescing || !list_empty(&ep->qh.queue) ||
		    !list_empty(&ep->rw_queue) || ep->last_zptr)
			goto out;
	}
	udc->c5_remove_state = C5_REMOVE_PREPARED;
	ret = 0;
out:
	spin_unlock_irqrestore(&udc_lock, flags);
	return ret;
}
#endif

int ci13xxx_msm_remove(struct platform_device *pdev)
{
	unsigned long flags;
	int ret;
	ret = ci13xxx_msm_retire(pdev);
	spin_lock_irqsave(&udc_lock, flags);
	_udc_ctxt.irq_ready = false;
	spin_unlock_irqrestore(&udc_lock, flags);
	free_irq(_udc_ctxt.irq, pdev);
	ci13xxx_msm_uninstall_wake_gpio(pdev, ret != 0);
	pm_runtime_disable(&pdev->dev);
	if (ret) {
		WRITE_ONCE(ci13xxx_parked, true);
		c5_otg_park_usb(&pdev->dev);
		return ret;
	}
	udc_release();
	iounmap(_udc_ctxt.regs);
	c5_otg_finish_child_remove(&pdev->dev);
	return 0;
}

void ci13xxx_msm_shutdown(struct platform_device *pdev)
{
	ci13xxx_pullup(&_udc->gadget, 0);
}

void msm_hw_soft_reset(void)
{
	struct ci13xxx *udc = _udc;

	hw_device_reset(udc);
}

void msm_hw_bam_disable(bool bam_disable)
{
	u32 val;
	struct ci13xxx *udc = _udc;

	if (bam_disable)
		val = readl_relaxed(USB_GENCONFIG) | GENCONFIG_BAM_DISABLE;
	else
		val = readl_relaxed(USB_GENCONFIG) & ~GENCONFIG_BAM_DISABLE;

	writel_relaxed(val, USB_GENCONFIG);
}

void msm_usb_irq_disable(bool disable)
{
	struct ci13xxx *udc;
	unsigned long flags;

	spin_lock_irqsave(&udc_lock, flags);
	udc = _udc;
	if (!udc || (udc->dma_owner && device_c5_usb_retained(udc->dma_owner)) || !_udc_ctxt.irq_ready ||
	    udc->c5_remove_state != C5_REMOVE_IDLE) {
		spin_unlock_irqrestore(&udc_lock, flags);
		return;
	}

	if (_udc_ctxt.irq_disabled == disable) {
		pr_debug("Interrupt state already disable = %d\n", disable);
		if (disable)
			mod_timer(&_udc_ctxt.irq_enable_timer,
				  IRQ_ENABLE_DELAY);
		spin_unlock_irqrestore(&udc_lock, flags);
		return;
	}

	if (disable) {
		disable_irq_nosync(_udc_ctxt.irq);
		/* start timer here */
		pr_debug("%s: Disabling interrupts\n", __func__);
		mod_timer(&_udc_ctxt.irq_enable_timer, IRQ_ENABLE_DELAY);
		_udc_ctxt.irq_disabled = true;

	} else {
		pr_debug("%s: Enabling interrupts\n", __func__);
		del_timer(&_udc_ctxt.irq_enable_timer);
		enable_irq(_udc_ctxt.irq);
		_udc_ctxt.irq_disabled = false;
	}

	spin_unlock_irqrestore(&udc_lock, flags);
}

static void enable_usb_irq_timer_func(unsigned long data)
{
	pr_debug("enabling interrupt from timer\n");
	msm_usb_irq_disable(false);
}

static struct msm_usb_child_driver ci13xxx_msm_driver = {
#ifdef CONFIG_C5_USB_MUIC_ROLE
    .claim_idle = ci13xxx_msm_claim_idle,
#endif
    .platform = {
	.probe = ci13xxx_msm_probe,
	.driver = {
#ifdef CONFIG_C5_USB_MUIC_ROLE
	    .prepare_remove = c5_otg_prepare_child_remove,
#endif
	    .name = "msm_hsusb",
	},
	.remove = ci13xxx_msm_remove,
	.shutdown = ci13xxx_msm_shutdown,
    },
};
MODULE_ALIAS("platform:msm_hsusb");

static int __init ci13xxx_msm_init(void)
{
	return platform_driver_register(&ci13xxx_msm_driver.platform);
}
module_init(ci13xxx_msm_init);

static void __exit ci13xxx_msm_exit(void)
{
	platform_driver_unregister(&ci13xxx_msm_driver.platform);
}
module_exit(ci13xxx_msm_exit);

MODULE_LICENSE("GPL v2");
