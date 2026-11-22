/* SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from legacy USB serial TTY modem handling:
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 * Delegate only to the connected function while its port lifetime is locked.
 */
#ifdef CONFIG_C5_USB_TTY_MODEM_IOCTLS
#ifdef CONFIG_MODULES
#error C5 TTY modem ioctl bridge is qualified only for the built-in profile
#endif
static const struct tty_operations gs_tty_ops;

bool c5_gserial_modem_tty(const struct tty_struct *tty)
{
    return tty && tty->ops == &gs_tty_ops;
}

static int c5_gs_tiocmget(struct tty_struct *tty)
{
    struct gs_port *port = tty->driver_data;
    struct gserial *gser;
    unsigned long flags;
    int result = 0;

    if (!port)
        return -ENODEV;
    spin_lock_irqsave(&port->port_lock, flags);
    gser = port->port_usb;
    if (!gser) {
        result = -ENODEV;
        goto out;
    }
    if (gser->get_dtr)
        result |= gser->get_dtr(gser) ? TIOCM_DTR : 0;
    if (gser->get_rts)
        result |= gser->get_rts(gser) ? TIOCM_RTS : 0;
    if (gser->serial_state & TIOCM_CD)
        result |= TIOCM_CD;
    if (gser->serial_state & TIOCM_RI)
        result |= TIOCM_RI;
out:
    spin_unlock_irqrestore(&port->port_lock, flags);
    return result;
}

static int c5_gs_tiocmset(struct tty_struct *tty,
                        unsigned int set, unsigned int clear)
{
    struct gs_port *port = tty->driver_data;
    struct gserial *gser;
    unsigned long flags;
    int status = 0;

    if (!port)
        return -ENODEV;
    spin_lock_irqsave(&port->port_lock, flags);
    gser = port->port_usb;
    if (!gser) {
        status = -ENODEV;
        goto out;
    }
    /* Preserve the old order, requested cache and last-callback result. */
    if ((set & TIOCM_RI) && gser->send_ring_indicator) {
        gser->serial_state |= TIOCM_RI;
        status = gser->send_ring_indicator(gser, 1);
    }
    if ((clear & TIOCM_RI) && gser->send_ring_indicator) {
        gser->serial_state &= ~TIOCM_RI;
        status = gser->send_ring_indicator(gser, 0);
    }
    if ((set & TIOCM_CD) && gser->send_carrier_detect) {
        gser->serial_state |= TIOCM_CD;
        status = gser->send_carrier_detect(gser, 1);
    }
    if ((clear & TIOCM_CD) && gser->send_carrier_detect) {
        gser->serial_state &= ~TIOCM_CD;
        status = gser->send_carrier_detect(gser, 0);
    }
out:
    spin_unlock_irqrestore(&port->port_lock, flags);
    return status;
}
#endif
