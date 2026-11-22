/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/irqdesc.h>
#include <linux/rcupdate.h>

/* irqdesc.c retires sparse descriptors with call_rcu. manage.c unlinks an
 * action under desc->lock, then frees it after dropping that lock. Copy the
 * action-owned name while both protections apply, under the normal IRQ API
 * name-owner lifetime contract. No action pointer leaves this function.
 */
static bool c5_resume_irq_name(unsigned int irq, char *name, size_t capacity)
{
 struct irq_desc *desc;
 unsigned long flags;
 bool named = false;
 rcu_read_lock();
 desc = irq_to_desc(irq);
 if (!desc) {
  scnprintf(name, capacity, "%s", "stray irq");
 } else {
  raw_spin_lock_irqsave(&desc->lock, flags);
  if (desc->action && desc->action->name) {
   scnprintf(name, capacity, "%s", desc->action->name);
   named = true;
  } else {
   scnprintf(name, capacity, "%s", "null");
  }
  raw_spin_unlock_irqrestore(&desc->lock, flags);
 }
 rcu_read_unlock();
 return named;
}

void c5_resume_summary_record_gic(unsigned int irq, unsigned int hwirq)
{
 char name[C5_RESUME_SUMMARY_SIZE];
 char record[C5_RESUME_SUMMARY_SIZE];
 c5_resume_irq_name(irq, name, sizeof(name));
 scnprintf(record, sizeof(record), "%d,%d,%s|", (int)irq, (int)hwirq, name);
 c5_resume_summary_append_record(record, false);
 pr_info("Resume caused by IRQ %d(GIC %d) %s\n", (int)irq, (int)hwirq, name);
}

void c5_resume_summary_record_gpio(unsigned int irq, unsigned long gpio)
{
 char name[C5_RESUME_SUMMARY_SIZE];
 char record[C5_RESUME_SUMMARY_SIZE];
 unsigned long flags;
 bool armed, named;
 /* Ordinary GPIO traffic does not enter descriptor/name processing. */
 raw_spin_lock_irqsave(&c5_resume_summary_lock, flags);
 armed = c5_resume_gpio_armed;
 raw_spin_unlock_irqrestore(&c5_resume_summary_lock, flags);
 if (!armed)
  return;
 named = c5_resume_irq_name(irq, name, sizeof(name));
 if (named)
  scnprintf(record, sizeof(record), "%u,%lu,%s|", irq, gpio, name);
 else
  scnprintf(record, sizeof(record), "%u,%lu|", irq, gpio);
 /* Recheck and consume under the same lock as the bounded append. */
 if (!c5_resume_summary_append_record(record, true))
  return;
 if (named)
  pr_info("Resume caused by IRQ %u(GPIO %lu) %s\n", irq, gpio, name);
 else
  pr_info("Resume caused by IRQ %u(GPIO %lu)\n", irq, gpio);
}

/* Format directly into the reader-owned output while descriptor/action
 * protection applies. This preserves the full sysfs page capacity without
 * keeping action pointers, allocating memory, or a page-sized stack array.
 * Callers must hold neither the summary lock nor the generic list lock.
 */
static size_t c5_resume_irq_format(int irq, char *destination, size_t capacity,
                                  bool as_log)
{
 struct irq_desc *desc;
 unsigned long flags;
 size_t length;
 rcu_read_lock();
 desc = irq_to_desc(irq);
 if (desc)
  raw_spin_lock_irqsave(&desc->lock, flags);
 if (desc && desc->action && desc->action->name)
  length = scnprintf(destination, capacity,
   as_log ? "Resume caused by IRQ %d, %s\n" : "%d %s\n",
   irq, desc->action->name);
 else
  length = scnprintf(destination, capacity,
   as_log ? "Resume caused by IRQ %d\n" : "%d\n", irq);
 if (desc)
  raw_spin_unlock_irqrestore(&desc->lock, flags);
 rcu_read_unlock();
 return length;
}

#ifdef CONFIG_C5_PM_PMIC_SUMMARY
void c5_resume_summary_record_pmic(unsigned int irq, unsigned long hwirq)
{
 char name[C5_RESUME_SUMMARY_SIZE];
 char record[C5_RESUME_SUMMARY_SIZE];
 c5_resume_irq_name(irq, name, sizeof(name));
 scnprintf(record, sizeof(record), "%u,%lu,%s|", irq, hwirq, name);
 c5_resume_summary_append_record(record, false);
 pr_info("Resume caused by IRQ %u(PIN %lu) %s [0x%lx, 0x%02lx,0x%lx]\n",
         irq, hwirq, name, (hwirq >> 11) & 0xf,
         (hwirq >> 3) & 0xff, hwirq & 7);
}
#endif
