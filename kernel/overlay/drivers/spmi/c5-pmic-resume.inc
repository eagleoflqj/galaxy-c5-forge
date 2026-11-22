/* SPDX-License-Identifier: GPL-2.0 */
/* Resume-only observation: no acknowledgement, cleanup or dispatch. */
static u32 c5_pmic_published;
static u32 c5_pmic_observations;
static u32 c5_pmic_record_attempts;
static void c5_pmic_show_periph(struct spmi_pmic_arb *pa, u16 apid)
{
 u32 pending, id;
 unsigned int irq;
 u8 sid = (pa->apid_data[apid].ppid >> 8) & 0xf;
 u8 per = pa->apid_data[apid].ppid & 0xff;
 pending = readl_relaxed(pa->intr + pa->ver_ops->irq_status(apid)) & 0xff;
 while (pending) {
  id = ffs(pending) - 1;
  pending &= ~BIT(id);
  irq = irq_find_mapping(pa->domain, HWIRQ(sid, per, id, apid));
  /* Preserve old show-only numeric attribution, including unmapped IRQ0.
   * It never enters native cleanup_irq or generic_handle_irq.
   */
  if (c5_pmic_record_attempts != ~0U)
   ++c5_pmic_record_attempts;
  c5_resume_summary_record_pmic(irq, ((unsigned long)sid << 11) |
                               ((unsigned long)per << 3) | id);
 }
}

static void c5_pmic_show_pending(struct spmi_pmic_arb *pa)
{
 u32 status, enable;
 int group, id, apid;
 bool acc_valid = false;
 /* Native unpopulated domain starts with inverted min/max. */
 if (pa->min_apid > pa->max_apid || pa->max_apid >= PMIC_ARB_MAX_PERIPHS)
  return;
 for (group = pa->min_apid >> 5; group <= pa->max_apid >> 5; ++group) {
  status = readl_relaxed(pa->acc_status +
           pa->ver_ops->owner_acc_status(pa->ee, group));
  if (status)
   acc_valid = true;
  while (status) {
   id = ffs(status) - 1;
   status &= ~BIT(id);
   apid = group * 32 + id;
   if (apid < pa->min_apid || apid > pa->max_apid)
    continue;
   enable = readl_relaxed(pa->intr + pa->ver_ops->acc_enable(apid));
   if (enable & SPMI_PIC_ACC_ENABLE_BIT)
    c5_pmic_show_periph(pa, apid);
  }
 }
 if (!acc_valid) {
  for (apid = pa->min_apid; apid <= pa->max_apid; ++apid) {
   if (pa->apid_data[apid].irq_owner != pa->ee)
    continue;
   status = readl_relaxed(pa->intr + pa->ver_ops->irq_status(apid));
   if (!status)
    continue;
   enable = readl_relaxed(pa->intr + pa->ver_ops->acc_enable(apid));
   if (enable & SPMI_PIC_ACC_ENABLE_BIT)
    c5_pmic_show_periph(pa, apid);
  }
 }
}

/* The lock covers the whole observation. Remove must unlink before tearing
 * down domain/controller/devres; no controller pointer survives the lock.
 * This is not the transaction lock pa->lock and has no reverse acquisition.
 */
static LIST_HEAD(c5_pmic_resume_devices);
static DEFINE_RAW_SPINLOCK(c5_pmic_resume_lock);

static void c5_pmic_resume_publish(struct spmi_pmic_arb *pa)
{
 unsigned long flags;
 raw_spin_lock_irqsave(&c5_pmic_resume_lock, flags);
 list_add_tail(&pa->c5_resume_node, &c5_pmic_resume_devices);
 ++c5_pmic_published;
 raw_spin_unlock_irqrestore(&c5_pmic_resume_lock, flags);
}

static void c5_pmic_resume_unpublish(struct spmi_pmic_arb *pa)
{
 unsigned long flags;
 raw_spin_lock_irqsave(&c5_pmic_resume_lock, flags);
 list_del_init(&pa->c5_resume_node);
 --c5_pmic_published;
 raw_spin_unlock_irqrestore(&c5_pmic_resume_lock, flags);
}

static void c5_pmic_syscore_resume(void)
{
 struct spmi_pmic_arb *pa;
 unsigned long flags;
 if (!READ_ONCE(msm_show_resume_irq_mask))
  return;
 raw_spin_lock_irqsave(&c5_pmic_resume_lock, flags);
 list_for_each_entry(pa, &c5_pmic_resume_devices, c5_resume_node) {
  if (c5_pmic_observations != ~0U)
   ++c5_pmic_observations;
  c5_pmic_show_pending(pa);
 }
 raw_spin_unlock_irqrestore(&c5_pmic_resume_lock, flags);
}

static struct syscore_ops c5_pmic_syscore_ops = {
 .resume = c5_pmic_syscore_resume,
};

/* Cached verification only. This getter never traverses controllers or MMIO. */
static int c5_pmic_resume_state_get(char *buffer, const struct kernel_param *kp)
{
 unsigned long flags;
 u32 published, observations, attempts;
 (void)kp;
 raw_spin_lock_irqsave(&c5_pmic_resume_lock, flags);
 published = c5_pmic_published;
 observations = c5_pmic_observations;
 attempts = c5_pmic_record_attempts;
 raw_spin_unlock_irqrestore(&c5_pmic_resume_lock, flags);
 return scnprintf(buffer, PAGE_SIZE,
                  "registered=%u observations=%u record_attempts=%u",
                  published, observations, attempts);
}

static const struct kernel_param_ops c5_pmic_resume_state_ops = {
 .get = c5_pmic_resume_state_get,
};
module_param_cb(c5_pmic_resume_state, &c5_pmic_resume_state_ops, NULL, 0400);
