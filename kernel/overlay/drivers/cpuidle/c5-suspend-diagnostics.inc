/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/sec-pinmux.h>
#include <linux/qpnp/pin.h>

/* The RPM provider already supports the old IRQ-disabled post-unprepare phase. */
extern int sec_print_masters_stats(void);
/* QPNP diagnostic declaration is provided by the selected native pin header. */
static int msm_pm_sleep_sec_debug;
module_param_named(secdebug, msm_pm_sleep_sec_debug, int, 0664);
static DEFINE_RAW_SPINLOCK(c5_suspend_diag_lock);
static u32 c5_diag_prepares, c5_diag_regulators, c5_diag_gpios, c5_diag_qpnps;
static u32 c5_diag_rpms, c5_diag_rpm_errors;
static int c5_diag_last_rpm_rc;

static noinline void c5_suspend_diag_prepare(void)
{
 unsigned long flags;
 bool gpio_qpnp = !!READ_ONCE(msm_pm_sleep_sec_debug);

 regulator_showall_enabled();
 if (gpio_qpnp) {
  msm_gpio_print_enabled();
  qpnp_debug_suspend_show();
 }
 raw_spin_lock_irqsave(&c5_suspend_diag_lock, flags);
 if (c5_diag_prepares != ~0U)
  c5_diag_prepares++;
 if (c5_diag_regulators != ~0U)
  c5_diag_regulators++;
 if (gpio_qpnp) {
  if (c5_diag_gpios != ~0U)
   c5_diag_gpios++;
  if (c5_diag_qpnps != ~0U)
   c5_diag_qpnps++;
 }
 raw_spin_unlock_irqrestore(&c5_suspend_diag_lock, flags);
}

static noinline void c5_suspend_diag_resume(void)
{
 unsigned long flags;
 int rc = sec_print_masters_stats();

 raw_spin_lock_irqsave(&c5_suspend_diag_lock, flags);
 if (c5_diag_rpms != ~0U)
  c5_diag_rpms++;
 if (rc && c5_diag_rpm_errors != ~0U)
  c5_diag_rpm_errors++;
 c5_diag_last_rpm_rc = rc;
 raw_spin_unlock_irqrestore(&c5_suspend_diag_lock, flags);
}

static int c5_suspend_diag_state_get(char *buf, const struct kernel_param *kp)
{
 unsigned long flags;
 u32 prepares, regulators, gpios, qpnps, rpms, errors;
 int rc;

 raw_spin_lock_irqsave(&c5_suspend_diag_lock, flags);
 prepares = c5_diag_prepares;
 regulators = c5_diag_regulators;
 gpios = c5_diag_gpios;
 qpnps = c5_diag_qpnps;
 rpms = c5_diag_rpms;
 errors = c5_diag_rpm_errors;
 rc = c5_diag_last_rpm_rc;
 raw_spin_unlock_irqrestore(&c5_suspend_diag_lock, flags);
 return scnprintf(buf, PAGE_SIZE,
  "C5_SUSPEND_DIAG1 ready=1 secdebug=%d prepares=%u regulators=%u gpios=%u qpnps=%u rpms=%u rpm_errors=%u last_rpm_rc=%d\n",
  READ_ONCE(msm_pm_sleep_sec_debug), prepares, regulators, gpios, qpnps, rpms, errors, rc);
}
static const struct kernel_param_ops c5_suspend_diag_state_ops = {
 .get = c5_suspend_diag_state_get,
};
module_param_cb(c5_suspend_diag_state, &c5_suspend_diag_state_ops, NULL, 0400);
