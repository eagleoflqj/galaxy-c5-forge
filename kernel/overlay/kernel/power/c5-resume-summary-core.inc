/* SPDX-License-Identifier: GPL-2.0 */
/* Private bounded resume-summary core. All mutable state shares one IRQ-safe lock. */
#define C5_RESUME_SUMMARY_SIZE 512
static char c5_resume_summary[C5_RESUME_SUMMARY_SIZE];
static size_t c5_resume_summary_length;
static bool c5_resume_gpio_armed;
static DEFINE_RAW_SPINLOCK(c5_resume_summary_lock);

static void c5_resume_summary_reset(void)
{
 unsigned long flags;
 raw_spin_lock_irqsave(&c5_resume_summary_lock, flags);
 c5_resume_summary_length = 0;
 c5_resume_summary[0] = '\0';
 c5_resume_gpio_armed = false;
 raw_spin_unlock_irqrestore(&c5_resume_summary_lock, flags);
}

void c5_resume_summary_arm_gpio(void)
{
 unsigned long flags;
 raw_spin_lock_irqsave(&c5_resume_summary_lock, flags);
 c5_resume_gpio_armed = true;
 raw_spin_unlock_irqrestore(&c5_resume_summary_lock, flags);
}

void c5_resume_summary_disarm_gpio(void)
{
 unsigned long flags;
 raw_spin_lock_irqsave(&c5_resume_summary_lock, flags);
 c5_resume_gpio_armed = false;
 raw_spin_unlock_irqrestore(&c5_resume_summary_lock, flags);
}

/* A caller must copy any IRQ action name while holding its descriptor lock,
 * then release that lock before calling here. No IRQ descriptor lock is taken
 * under this lock. The copied record contains no pointer to a live action.
 */
bool c5_resume_summary_append_record(const char *record, bool first_gpio)
{
 unsigned long flags;
 bool accepted = false;
 raw_spin_lock_irqsave(&c5_resume_summary_lock, flags);
 if (first_gpio && !c5_resume_gpio_armed)
  goto out;
 if (first_gpio)
  c5_resume_gpio_armed = false;
 c5_resume_summary_length += scnprintf(
  c5_resume_summary + c5_resume_summary_length,
  sizeof(c5_resume_summary) - c5_resume_summary_length, "%s", record);
 accepted = true;
out:
 raw_spin_unlock_irqrestore(&c5_resume_summary_lock, flags);
 return accepted;
}

static size_t c5_resume_summary_copy(char *destination, size_t capacity)
{
 unsigned long flags;
 size_t count;
 raw_spin_lock_irqsave(&c5_resume_summary_lock, flags);
 count = capacity ? min(c5_resume_summary_length, capacity - 1) : 0;
 if (capacity) {
  memcpy(destination, c5_resume_summary, count);
  destination[count] = '\0';
 }
 raw_spin_unlock_irqrestore(&c5_resume_summary_lock, flags);
 return count;
}
