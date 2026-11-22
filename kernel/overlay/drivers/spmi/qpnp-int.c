// SPDX-License-Identifier: GPL-2.0-only
/* Legacy software-only diagnostic mask ABI from drivers/spmi/qpnp-int.c.
 * Native PMIC observation uses the separate msm_show_resume_irq_mask.
 * This value has no hardware, IRQ or resume callback consumer.
 */
#include <linux/moduleparam.h>
#ifdef CONFIG_C5_PM_QPNP_INT_MASK
int msm_qpnp_int_debug_mask;
module_param_named(debug_mask, msm_qpnp_int_debug_mask, int, 0664);
#endif
