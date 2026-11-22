/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/interrupt.h>
#include <linux/moduleparam.h>

#define C5_REG_MAX 256
#define C5_REG_DEPTH 16
#define C5_REG_NAME 64
#define C5_REG_CLIENTS 80
#define C5_REG_CONSUMERS 256
struct c5_reg_row {
 char name[C5_REG_NAME], consumers[C5_REG_CLIENTS];
 int enabled, voltage, mode;
 unsigned int errors;
 bool voltage_supported, mode_supported;
};
struct c5_reg_parent { struct device *dev; int status; bool locked; };
struct c5_reg_snapshot {
 unsigned int entries, enabled, errors, busy, unbound, parents;
 struct c5_reg_parent parent[C5_REG_MAX];
 u8 eligible[C5_REG_MAX];
 struct c5_reg_row rows[C5_REG_MAX];
};
static DEFINE_MUTEX(c5_reg_lock);
static struct regulator_dev *c5_reg_registry[C5_REG_MAX];
static u32 c5_reg_registered, c5_reg_overflows, c5_reg_calls;
static u32 c5_reg_last_entries, c5_reg_last_enabled, c5_reg_last_errors;
static u32 c5_reg_last_busy, c5_reg_last_unbound;
static int c5_reg_last_rc;
void regulator_showall_enabled(void);

static noinline void c5_reg_publish(struct regulator_dev *rdev)
{
 unsigned int i, free = C5_REG_MAX;
 mutex_lock(&c5_reg_lock);
 for (i = 0; i < C5_REG_MAX; i++) {
  if (c5_reg_registry[i] == rdev)
   goto lost;
  if (!c5_reg_registry[i] && free == C5_REG_MAX)
   free = i;
 }
 if (free == C5_REG_MAX)
  goto lost;
 c5_reg_registry[free] = rdev;
 c5_reg_registered++;
 mutex_unlock(&c5_reg_lock);
 return;
lost:
 if (c5_reg_overflows != ~0U)
  c5_reg_overflows++;
 mutex_unlock(&c5_reg_lock);
}
static noinline void c5_reg_retire(struct regulator_dev *rdev)
{
 unsigned int i;
 mutex_lock(&c5_reg_lock);
 for (i = 0; i < C5_REG_MAX; i++)
  if (c5_reg_registry[i] == rdev) {
   c5_reg_registry[i] = NULL;
   c5_reg_registered--;
   break;
  }
 mutex_unlock(&c5_reg_lock);
}

/* Registry ownership keeps rdev and its parent-device reference alive.
 * Try, never wait for, provider lifecycle locks before dereferencing driver state.
 * Keep all successfully bound parents locked through every supply-chain query.
 */
static void c5_reg_prepare_parents(struct c5_reg_snapshot *s)
{
 unsigned int i, j;
 for (i = 0; i < C5_REG_MAX; i++) {
  struct regulator_dev *rdev = c5_reg_registry[i];
  struct device *parent;
  struct c5_reg_parent *p;
  if (!rdev)
   continue;
  parent = rdev->dev.parent;
  if (!parent) {
   s->eligible[i] = rdev == dummy_regulator_rdev ? 0 : 2;
   continue;
  }
  for (j = 0; j < s->parents; j++)
   if (s->parent[j].dev == parent)
    break;
  if (j == s->parents) {
   p = &s->parent[s->parents++];
   p->dev = parent;
   if (!device_trylock(parent)) {
    p->status = 1;
   } else if (!parent->driver) {
    p->status = 2;
    device_unlock(parent);
   } else {
    p->locked = true;
   }
  }
  s->eligible[i] = s->parent[j].status;
 }
}
static void c5_reg_release_parents(struct c5_reg_snapshot *s)
{
 while (s->parents) {
  struct c5_reg_parent *p = &s->parent[--s->parents];
  if (p->locked)
   device_unlock(p->dev);
  p->dev = NULL;
  p->locked = false;
 }
}
static int c5_reg_readable(struct regulator_dev *rdev,
                          struct c5_reg_snapshot *s, unsigned int *blocked)
{
 unsigned int i;
 for (i = 0; i < C5_REG_MAX; i++)
  if (c5_reg_registry[i] == rdev) {
   if (s->eligible[i]) {
    *blocked = s->eligible[i];
    return s->eligible[i] == 1 ? -EAGAIN : -ENODEV;
   }
   return 0;
  }
 *blocked = 2;
 return -ENODEV;
}
/* Keep native voltage-query order and offset semantics, with bounded supply descent. */
static noinline int c5_reg_voltage(struct regulator_dev *rdev, unsigned int depth, bool *supported,
                                   struct c5_reg_snapshot *snapshot, unsigned int *blocked)
{
 const struct regulator_ops *ops;
 int sel, ret;
 bool bypassed;
 if (depth == C5_REG_DEPTH)
  return -ELOOP;
 if (!rdev)
  return -ENODEV;
 ret = c5_reg_readable(rdev, snapshot, blocked);
 if (ret)
  return ret;
 if (!rdev->desc || !rdev->constraints)
  return -ENODEV;
 ops = rdev->desc->ops;
 if (!ops)
  return -EIO;
 if (ops->get_bypass) {
  ret = ops->get_bypass(rdev, &bypassed);
  if (ret < 0)
   return ret;
  if (bypassed) {
   if (!rdev->supply)
    return -EPROBE_DEFER;
   return c5_reg_voltage(rdev->supply->rdev, depth + 1, supported, snapshot, blocked);
  }
 }
 if (ops->get_voltage_sel) {
  sel = ops->get_voltage_sel(rdev);
  if (sel < 0)
   return sel;
  if (!ops->list_voltage)
   return -EIO;
  ret = ops->list_voltage(rdev, sel);
 } else if (ops->get_voltage) {
  ret = ops->get_voltage(rdev);
 } else if (ops->list_voltage) {
  ret = ops->list_voltage(rdev, 0);
 } else if (rdev->desc->fixed_uV && rdev->desc->n_voltages == 1) {
  ret = rdev->desc->fixed_uV;
 } else if (rdev->supply) {
  ret = c5_reg_voltage(rdev->supply->rdev, depth + 1, supported, snapshot, blocked);
 } else if (rdev->supply_name) {
  return -EPROBE_DEFER;
 } else {
  *supported = false;
  return 0;
 }
 if (ret < 0)
  return ret;
 if (!*supported)
  return 0;
 /* Avoid signed overflow in malformed metadata without altering regulation. */
 if ((rdev->constraints->uV_offset > 0 && ret < INT_MIN + rdev->constraints->uV_offset) ||
     (rdev->constraints->uV_offset < 0 && ret > INT_MAX + rdev->constraints->uV_offset))
  return -ERANGE;
 return ret - rdev->constraints->uV_offset;
}
static int c5_reg_copy(struct c5_reg_snapshot *s)
{
 unsigned int i;
 int rc = 0;
 mutex_lock(&c5_reg_lock);
 if (!c5_reg_registered || c5_reg_overflows) {
  rc = c5_reg_overflows ? -EOVERFLOW : -ENODEV;
  goto unlock;
 }
 c5_reg_prepare_parents(s);
 for (i = 0; i < C5_REG_MAX; i++) {
  struct regulator_dev *rdev = c5_reg_registry[i];
  struct c5_reg_row *row;
  const struct regulator_ops *ops;
  struct regulator *consumer;
  unsigned int length = 0, seen = 0, blocked = 0;
  const char *name;
  if (!rdev)
   continue;
  if (s->entries == C5_REG_MAX) {
   rc = -EOVERFLOW;
   break;
  }
  row = &s->rows[s->entries++];
  if (s->eligible[i] || !mutex_trylock(&rdev->mutex)) {
   row->enabled = s->eligible[i] == 2 ? -ENODEV : -EAGAIN;
   if (s->eligible[i] == 2)
    s->unbound++;
   else
    s->busy++;
   row->errors++;
   s->errors++;
   continue;
  }
  if (!rdev->desc || !rdev->desc->ops || !rdev->constraints) {
   row->errors++;
   goto row_done;
  }
  name = rdev->constraints->name ? rdev->constraints->name : rdev->desc->name;
  strlcpy(row->name, name ? name : "", sizeof(row->name));
  ops = rdev->desc->ops;
  row->enabled = rdev->ena_pin ? rdev->ena_gpio_state :
   (ops->is_enabled ? ops->is_enabled(rdev) : 1);
  if (row->enabled < 0) {
   row->errors++;
   goto row_done;
  }
  if (!row->enabled)
   goto row_done;
  s->enabled++;
  row->voltage_supported = true;
  row->voltage = c5_reg_voltage(rdev, 0, &row->voltage_supported, s, &blocked);
  if (row->voltage_supported && row->voltage < 0)
   row->errors++;
  if (blocked == 1)
   s->busy++;
  else if (blocked == 2)
   s->unbound++;
  row->mode_supported = !!ops->get_mode;
  /* The public helper takes rdev->mutex itself: call its native operation here. */
  if (row->mode_supported) {
   row->mode = (int)ops->get_mode(rdev);
   if (row->mode < 0)
    row->errors++;
  }
  list_for_each_entry(consumer, &rdev->consumer_list, list) {
   unsigned int n;
   if (++seen > C5_REG_CONSUMERS) {
    row->errors++;
    break;
   }
   if (!consumer->enabled || !consumer->supply_name)
    continue;
   n = strnlen(consumer->supply_name, C5_REG_CLIENTS);
   if (n == C5_REG_CLIENTS || length + n + 3 > C5_REG_CLIENTS)
    break;
   length += scnprintf(row->consumers + length, sizeof(row->consumers) - length,
                       ", %s", consumer->supply_name);
  }
row_done:
  s->errors += row->errors;
  mutex_unlock(&rdev->mutex);
 }
unlock:
 c5_reg_release_parents(s);
 mutex_unlock(&c5_reg_lock);
 return rc;
}
static int c5_reg_dump(void)
{
 struct c5_reg_snapshot *s;
 unsigned int i;
 int rc;
 if (WARN_ON_ONCE(in_atomic() || irqs_disabled()))
  return -EWOULDBLOCK;
 s = kzalloc(sizeof(*s), GFP_KERNEL);
 if (!s)
  return -ENOMEM;
 rc = c5_reg_copy(s);
 if (!rc) {
  printk(KERN_INFO "---Enabled regulators---\n");
  for (i = 0; i < s->entries; i++) {
   struct c5_reg_row *row = &s->rows[i];
   if (row->enabled > 0)
    printk(KERN_INFO "%s: native_voltage=%d supported=%u native_mode=%d supported=%u errors=%u%s\n",
       row->name, row->voltage, row->voltage_supported, row->mode,
       row->mode_supported, row->errors,
       row->consumers[0] ? row->consumers : ", null");
  }
  printk(KERN_INFO "---Enabled regulator count: %u---\n", s->enabled);
  if (s->errors)
   rc = -EUCLEAN;
 }
 mutex_lock(&c5_reg_lock);
 if ((!rc || rc == -EUCLEAN) && c5_reg_calls != ~0U)
  c5_reg_calls++;
 c5_reg_last_entries = s->entries;
 c5_reg_last_enabled = s->enabled;
 c5_reg_last_errors = s->errors;
 c5_reg_last_busy = s->busy;
 c5_reg_last_unbound = s->unbound;
 c5_reg_last_rc = rc;
 mutex_unlock(&c5_reg_lock);
 kfree(s);
 return rc;
}
void regulator_showall_enabled(void)
{
 c5_reg_dump();
}
static int c5_reg_state_get(char *buf, const struct kernel_param *kp)
{
 u32 registered, overflows, calls, entries, enabled, errors, busy, unbound;
 int rc;
 mutex_lock(&c5_reg_lock);
 registered = c5_reg_registered;
 overflows = c5_reg_overflows;
 calls = c5_reg_calls;
 entries = c5_reg_last_entries;
 enabled = c5_reg_last_enabled;
 errors = c5_reg_last_errors;
 busy = c5_reg_last_busy;
 unbound = c5_reg_last_unbound;
 rc = c5_reg_last_rc;
 mutex_unlock(&c5_reg_lock);
 return scnprintf(buf, PAGE_SIZE,
  "C5_REGULATOR1 ready=%u registered=%u overflows=%u calls=%u entries=%u enabled=%u busy=%u unbound=%u errors=%u rc=%d\n",
  !!registered && !overflows, registered, overflows, calls, entries, enabled, busy, unbound, errors, rc);
}
static int c5_reg_probe_get(char *buf, const struct kernel_param *kp)
{
 int rc = c5_reg_dump();
 return rc ? rc : c5_reg_state_get(buf, kp);
}
static const struct kernel_param_ops c5_reg_state_ops = { .get = c5_reg_state_get };
static const struct kernel_param_ops c5_reg_probe_ops = { .get = c5_reg_probe_get };
module_param_cb(c5_regulator_debug_state, &c5_reg_state_ops, NULL, 0400);
module_param_cb(c5_regulator_debug_probe, &c5_reg_probe_ops, NULL, 0400);
