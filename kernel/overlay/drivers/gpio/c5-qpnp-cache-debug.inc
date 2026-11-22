/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/interrupt.h>
#include <linux/moduleparam.h>

#define C5_QPNP_MAX_CONTROLLERS 4
#define C5_QPNP_MAX_ROWS 128
#define C5_QPNP_NAME 64
#define C5_QPNP_FIELDS 11
#include "c5-qpnp-cache-decoder.inc"

struct c5_qpnp_owner {
 struct list_head list;
 struct platform_device *pdev;
 struct qpnp_pin_chip *chip;
 unsigned int reserved_rows;
 char name[C5_QPNP_NAME];
};
struct c5_qpnp_row {
 unsigned int controller, number;
 u16 present, errors;
 u8 values[C5_QPNP_FIELDS];
};
struct c5_qpnp_snapshot {
 unsigned int controllers, rows, errors;
 char names[C5_QPNP_MAX_CONTROLLERS][C5_QPNP_NAME];
 struct c5_qpnp_row row[C5_QPNP_MAX_ROWS];
};
struct c5_qpnp_field {
 enum qpnp_pin_param_type type;
 const char * const *names;
 unsigned int count;
};
#define C5_QFIELD(id, table) { Q_PIN_CFG_##id, qpnp_##table##_str, ARRAY_SIZE(qpnp_##table##_str) }
static const struct c5_qpnp_field c5_qpnp_fields[C5_QPNP_FIELDS] = {
 C5_QFIELD(MODE, mode), C5_QFIELD(OUTPUT_TYPE, output_type),
 C5_QFIELD(INVERT, invert), C5_QFIELD(PULL, pull), C5_QFIELD(VIN_SEL, vin_sel),
 C5_QFIELD(OUT_STRENGTH, out_strength), C5_QFIELD(SRC_SEL, src_sel),
 C5_QFIELD(MASTER_EN, master_en), C5_QFIELD(AOUT_REF, aout_ref),
 C5_QFIELD(AIN_ROUTE, ain_route), C5_QFIELD(CS_OUT, cs_out),
};
static LIST_HEAD(c5_qpnp_owners);
static DEFINE_MUTEX(c5_qpnp_lock);
static u32 c5_qpnp_calls, c5_qpnp_last_rows, c5_qpnp_last_errors;
static int c5_qpnp_last_rc;

void qpnp_debug_suspend_show(void);

/* Keep the original callbacks address-taken and their return ABI intact. */
static int (* volatile c5_qpnp_native_probe)(struct platform_device *) = qpnp_pin_probe;
static int (* volatile c5_qpnp_native_remove)(struct platform_device *) = qpnp_pin_remove;

static unsigned int c5_qpnp_ready_locked(void)
{
 struct c5_qpnp_owner *o;
 unsigned int n = 0;
 list_for_each_entry(o, &c5_qpnp_owners, list)
  if (o->chip)
   n++;
 return n;
}

static int c5_qpnp_debug_probe(struct platform_device *pdev)
{
 struct c5_qpnp_owner *o, *entry;
 struct qpnp_pin_chip *chip;
 const char *name = dev_name(&pdev->dev);
 unsigned int controllers = 0, rows = 0, n, i;
 int rc;

 if (!pdev->dev.of_node || !name || !*name || strnlen(name, C5_QPNP_NAME) == C5_QPNP_NAME)
  return -EINVAL;
 n = of_get_available_child_count(pdev->dev.of_node);
 if (!n || n > C5_QPNP_MAX_ROWS)
  return -EINVAL;
 o = kzalloc(sizeof(*o), GFP_KERNEL);
 if (!o)
  return -ENOMEM;
 o->pdev = pdev;
 o->reserved_rows = n;
 strlcpy(o->name, name, sizeof(o->name));
 mutex_lock(&c5_qpnp_lock);
 list_for_each_entry(entry, &c5_qpnp_owners, list) {
  if (entry->pdev == pdev) {
   rc = -EBUSY;
   goto refuse_locked;
  }
  controllers++;
  rows += entry->reserved_rows;
 }
 if (controllers == C5_QPNP_MAX_CONTROLLERS || rows > C5_QPNP_MAX_ROWS - n) {
  rc = -E2BIG;
  goto refuse_locked;
 }
 /* Reserved before native probe, but not visible as a ready controller. */
 list_add_tail(&o->list, &c5_qpnp_owners);
 mutex_unlock(&c5_qpnp_lock);
 rc = c5_qpnp_native_probe(pdev);
 if (rc)
  goto withdraw;
 chip = dev_get_drvdata(&pdev->dev);
 if (!chip || !chip->chip_gpios || chip->gpio_chip.ngpio != n) {
  rc = -EINVAL;
  goto remove_native;
 }
 for (i = 0; i < n; i++) {
  struct qpnp_pin_spec *spec = chip->chip_gpios[i];
  if (!spec || !spec->num_ctl_regs || spec->num_ctl_regs > Q_NUM_CTL_REGS) {
   rc = -EINVAL;
   goto remove_native;
  }
 }
 mutex_lock(&c5_qpnp_lock);
 o->chip = chip;
 mutex_unlock(&c5_qpnp_lock);
 return 0;

refuse_locked:
 mutex_unlock(&c5_qpnp_lock);
 kfree(o);
 return rc;
remove_native:
 /* Native success implies a valid chip; never call its remove with NULL. */
 if (chip)
  c5_qpnp_native_remove(pdev);
withdraw:
 mutex_lock(&c5_qpnp_lock);
 list_del(&o->list);
 mutex_unlock(&c5_qpnp_lock);
 kfree(o);
 return rc;
}

static int c5_qpnp_debug_remove(struct platform_device *pdev)
{
 struct c5_qpnp_owner *o, *found = NULL;
 mutex_lock(&c5_qpnp_lock);
 list_for_each_entry(o, &c5_qpnp_owners, list)
  if (o->pdev == pdev) {
   found = o;
   list_del(&o->list);
   break;
  }
 mutex_unlock(&c5_qpnp_lock);
 if (!found)
  return -ENODEV;
 /* All new readers have released native chip/pin pointers before this. */
 kfree(found);
 return c5_qpnp_native_remove(pdev);
}

static int c5_qpnp_copy(struct c5_qpnp_snapshot *s)
{
 struct c5_qpnp_owner *o;
 unsigned int i, f;
 int rc = 0;
 mutex_lock(&c5_qpnp_lock);
 list_for_each_entry(o, &c5_qpnp_owners, list) {
  struct qpnp_pin_chip *chip = o->chip;
  unsigned int controller;
  if (!chip)
   continue;
  if (s->controllers >= C5_QPNP_MAX_CONTROLLERS || !chip->chip_gpios ||
      chip->gpio_chip.ngpio != o->reserved_rows ||
      o->reserved_rows > C5_QPNP_MAX_ROWS - s->rows) {
   rc = -EIO;
   break;
  }
  controller = s->controllers++;
  strlcpy(s->names[controller], o->name, sizeof(s->names[controller]));
  for (i = 0; i < o->reserved_rows; i++) {
   struct qpnp_pin_spec *spec = chip->chip_gpios[i];
   struct c5_qpnp_row *row = &s->row[s->rows++];
   row->controller = controller;
   row->number = i + 1;
   if (!spec || !spec->num_ctl_regs || spec->num_ctl_regs > Q_NUM_CTL_REGS) {
    rc = -EIO;
    goto unlock;
   }
   for (f = 0; f < C5_QPNP_FIELDS; f++) {
    struct c5_qpnp_reg cfg = {};
    unsigned int value;
    u8 byte;
    int support = c5_qpnp_check_config(c5_qpnp_fields[f].type, spec, 0);
    if (support == -ENXIO)
     continue;
    row->present |= BIT(f);
    /* EINVAL for a zero output-strength support probe still denotes a field. */
    if ((support && support != -EINVAL) ||
        c5_qpnp_reg_attr(c5_qpnp_fields[f].type, &cfg, spec) ||
        cfg.idx >= spec->num_ctl_regs || cfg.idx >= Q_NUM_CTL_REGS || cfg.shift >= 8) {
     row->errors |= BIT(f);
     s->errors++;
     continue;
    }
    /* Existing control caches may change concurrently: each byte, not a bus snapshot. */
    byte = READ_ONCE(spec->regs[cfg.idx]);
    value = c5_qpnp_reg_get(&byte, cfg.shift, cfg.mask);
    if (value >= c5_qpnp_fields[f].count) {
     row->errors |= BIT(f);
     s->errors++;
    } else {
     row->values[f] = value;
    }
   }
  }
 }
 if (!s->controllers && !rc)
  rc = -ENODEV;
unlock:
 mutex_unlock(&c5_qpnp_lock);
 return rc;
}

static int c5_qpnp_dump(void)
{
 struct c5_qpnp_snapshot *s;
 unsigned int i, f;
 int rc;
 if (WARN_ON_ONCE(in_atomic() || irqs_disabled()))
  return -EWOULDBLOCK;
 s = kzalloc(sizeof(*s), GFP_KERNEL);
 if (!s)
  return -ENOMEM;
 rc = c5_qpnp_copy(s);
 if (!rc) {
  for (i = 0; i < s->rows; i++) {
   struct c5_qpnp_row *row = &s->row[i];
   char line[256];
   int used = scnprintf(line, sizeof(line), "%s-%02u", s->names[row->controller], row->number);
   for (f = 0; f < C5_QPNP_FIELDS; f++)
    if (row->present & BIT(f))
     used += scnprintf(line + used, sizeof(line) - used, "%10s",
        row->errors & BIT(f) ? "ERR" : c5_qpnp_fields[f].names[row->values[f]]);
   printk(KERN_INFO "%s\n", line);
  }
  if (s->errors)
   rc = -EUCLEAN;
 }
 mutex_lock(&c5_qpnp_lock);
 if ((!rc || rc == -EUCLEAN) && c5_qpnp_calls != ~0U)
  c5_qpnp_calls++;
 c5_qpnp_last_rows = s->rows;
 c5_qpnp_last_errors = s->errors;
 c5_qpnp_last_rc = rc;
 mutex_unlock(&c5_qpnp_lock);
 kfree(s);
 return rc;
}

void qpnp_debug_suspend_show(void)
{
 c5_qpnp_dump();
}

static int c5_qpnp_state_get(char *buf, const struct kernel_param *kp)
{
 unsigned int ready;
 u32 calls, rows, errors;
 int rc;
 mutex_lock(&c5_qpnp_lock);
 ready = c5_qpnp_ready_locked();
 calls = c5_qpnp_calls;
 rows = c5_qpnp_last_rows;
 errors = c5_qpnp_last_errors;
 rc = c5_qpnp_last_rc;
 mutex_unlock(&c5_qpnp_lock);
 return scnprintf(buf, PAGE_SIZE,
  "C5_QPNP_CACHE1 ready=%u controllers=%u calls=%u rows=%u errors=%u rc=%d\n",
  !!ready, ready, calls, rows, errors, rc);
}
static int c5_qpnp_probe_get(char *buf, const struct kernel_param *kp)
{
 int rc = c5_qpnp_dump();
 return rc ? rc : c5_qpnp_state_get(buf, kp);
}
static const struct kernel_param_ops c5_qpnp_state_ops = { .get = c5_qpnp_state_get };
static const struct kernel_param_ops c5_qpnp_probe_ops = { .get = c5_qpnp_probe_get };
module_param_cb(c5_qpnp_debug_state, &c5_qpnp_state_ops, NULL, 0400);
module_param_cb(c5_qpnp_debug_probe, &c5_qpnp_probe_ops, NULL, 0400);
