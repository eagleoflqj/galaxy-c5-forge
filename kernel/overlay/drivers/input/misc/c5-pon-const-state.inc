/* SPDX-License-Identifier: GPL-2.0-only */
/* Queries use one static word, never borrowed provider memory. */
#define C5_PON_CONST_STORED BIT(0)
#define C5_PON_CONST_USES_C5 BIT(1)
#define C5_PON_CONST_READY BIT(31)
#define C5_PON_CONST_WARM_SHIFT 8
static DEFINE_SPINLOCK(c5_pon_const_lock);
static struct qpnp_pon *c5_pon_const_owner;
static u32 c5_pon_const_word, c5_pon_const_checks, c5_pon_const_errors;
static noinline int c5_pon_owned_warm_reset(struct qpnp_pon *pon)
{

	if (!pon)
		return -EPROBE_DEFER;

	if (is_pon_gen1(pon) || pon->subtype == PON_1REG)
		return pon->warm_reset_reason1
			|| (pon->warm_reset_reason2 & QPNP_PON_WARM_RESET_TFT);
	else
		return pon->warm_reset_reason1;
}
static noinline void c5_pon_const_publish(struct qpnp_pon *pon, bool primary)
{
 unsigned long flags;
 u32 value;
 if (!primary || !pon)
  return;
 value = C5_PON_CONST_READY |
  ((u32)c5_pon_owned_warm_reset(pon) << C5_PON_CONST_WARM_SHIFT);
 if (pon->store_hard_reset_reason) {
  value |= C5_PON_CONST_STORED;
  if (pon->c5lte_restart_reason)
   value |= C5_PON_CONST_USES_C5;
 }
 spin_lock_irqsave(&c5_pon_const_lock, flags);
 if (c5_pon_const_owner && c5_pon_const_owner != pon) {
  if (c5_pon_const_errors != ~0U)
   c5_pon_const_errors++;
 } else {
  c5_pon_const_owner = pon;
  WRITE_ONCE(c5_pon_const_word, value);
 }
 spin_unlock_irqrestore(&c5_pon_const_lock, flags);
}
static noinline void c5_pon_const_retire(struct qpnp_pon *pon)
{
 unsigned long flags;
 spin_lock_irqsave(&c5_pon_const_lock, flags);
 if (c5_pon_const_owner == pon) {
  WRITE_ONCE(c5_pon_const_word, 0);
  c5_pon_const_owner = NULL;
 }
 spin_unlock_irqrestore(&c5_pon_const_lock, flags);
}
static int c5_pon_const_info_get(char *buf, const struct kernel_param *kp)
{
 unsigned long flags;
 u32 value, checks, errors, ready;
 bool use_c5, stored;
 int warm, expected_warm;
 spin_lock_irqsave(&c5_pon_const_lock, flags);
 value = READ_ONCE(c5_pon_const_word);
 ready = !!(value & C5_PON_CONST_READY);
 use_c5 = qpnp_pon_uses_c5lte_restart_reason();
 stored = qpnp_pon_check_hard_reset_stored();
 warm = qpnp_pon_is_warm_reset();
 expected_warm = ready ? (int)((value >> C5_PON_CONST_WARM_SHIFT) & 0xff) : -EPROBE_DEFER;
 if (use_c5 != !!(value & C5_PON_CONST_USES_C5) ||
     stored != !!(value & C5_PON_CONST_STORED) || warm != expected_warm) {
  if (c5_pon_const_errors != ~0U)
   c5_pon_const_errors++;
 }
 if (c5_pon_const_checks > ~0U - 3)
  c5_pon_const_checks = ~0U;
 else
  c5_pon_const_checks += 3;
 checks = c5_pon_const_checks;
 errors = c5_pon_const_errors;
 spin_unlock_irqrestore(&c5_pon_const_lock, flags);
 return scnprintf(buf, PAGE_SIZE,
  "C5_PON_CONST1 ready=%u checks=%u errors=%u rc=0\n", ready, checks, errors);
}
static const struct kernel_param_ops c5_pon_const_info_ops = { .get = c5_pon_const_info_get };
module_param_cb(c5_pon_const_info, &c5_pon_const_info_ops, NULL, 0400);
