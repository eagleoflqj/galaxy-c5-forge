/* SPDX-License-Identifier: GPL-2.0-only */
/* A lookup reference does not impose a sleeping lock on reboot callers. */
static DEFINE_MUTEX(c5_pon_borrow_writer_lock);
static struct qpnp_pon *c5_pon_borrow_reserved;
static struct qpnp_pon __rcu *c5_pon_borrow_live;
static u32 c5_pon_borrow_publish_errors;

static void c5_pon_borrow_release(struct kref *ref)
{
 struct qpnp_pon *pon = container_of(ref, struct qpnp_pon, c5_borrow_users);
 /* No provider access after completion: the remover can now finish. */
 complete(&pon->c5_borrow_released);
}

static noinline struct qpnp_pon *c5_pon_borrow_get(void)
{
 struct qpnp_pon *pon;
 int users;
 rcu_read_lock();
 pon = rcu_dereference(c5_pon_borrow_live);
 if (pon) {
  users = atomic_read(&pon->c5_borrow_users.refcount);
  for (;;) {
   int old;
   if (users <= 0 || users == INT_MAX) {
    pon = ERR_PTR(-EOVERFLOW);
    break;
   }
   old = atomic_cmpxchg(&pon->c5_borrow_users.refcount, users, users + 1);
   if (old == users)
    break;
   users = old;
  }
 }
 rcu_read_unlock();
 return pon;
}

static noinline void c5_pon_borrow_put(struct qpnp_pon *pon)
{
 if (pon)
  kref_put(&pon->c5_borrow_users, c5_pon_borrow_release);
}

static noinline int c5_pon_borrow_reserve(struct qpnp_pon *pon, bool primary)
{
 int rc = 0;
 if (!primary)
  return 0;
 if (!pon)
  return -EINVAL;
 mutex_lock(&c5_pon_borrow_writer_lock);
 if (c5_pon_borrow_reserved) {
  rc = -EINVAL;
 } else {
  kref_init(&pon->c5_borrow_users);
  init_completion(&pon->c5_borrow_released);
  c5_pon_borrow_reserved = pon;
 }
 mutex_unlock(&c5_pon_borrow_writer_lock);
 return rc;
}

static noinline void c5_pon_borrow_publish(struct qpnp_pon *pon, bool primary)
{
 if (!primary || !pon)
  return;
 mutex_lock(&c5_pon_borrow_writer_lock);
 if (c5_pon_borrow_reserved == pon) {
  rcu_assign_pointer(c5_pon_borrow_live, pon);
 } else if (c5_pon_borrow_publish_errors != ~0U) {
  c5_pon_borrow_publish_errors++;
 }
 mutex_unlock(&c5_pon_borrow_writer_lock);
}

static noinline void c5_pon_borrow_retire(struct qpnp_pon *pon)
{
 mutex_lock(&c5_pon_borrow_writer_lock);
 if (pon && c5_pon_borrow_reserved == pon) {
  RCU_INIT_POINTER(c5_pon_borrow_live, NULL);
  synchronize_rcu();
  kref_put(&pon->c5_borrow_users, c5_pon_borrow_release);
  wait_for_completion(&pon->c5_borrow_released);
  c5_pon_borrow_reserved = NULL;
 }
 mutex_unlock(&c5_pon_borrow_writer_lock);
}

static int c5_pon_borrow_info_get(char *buf, const struct kernel_param *kp)
{
 u32 ready, users, errors;
 if (!mutex_trylock(&c5_pon_borrow_writer_lock))
  return -EBUSY;
 ready = c5_pon_borrow_reserved &&
  rcu_access_pointer(c5_pon_borrow_live) == c5_pon_borrow_reserved;
 users = c5_pon_borrow_reserved ?
  atomic_read(&c5_pon_borrow_reserved->c5_borrow_users.refcount) : 0;
 errors = c5_pon_borrow_publish_errors;
 mutex_unlock(&c5_pon_borrow_writer_lock);
 return scnprintf(buf, PAGE_SIZE,
  "C5_PON_BORROW1 ready=%u refs=%u errors=%u rc=0\n", ready, users, errors);
}
static const struct kernel_param_ops c5_pon_borrow_info_ops = { .get = c5_pon_borrow_info_get };
module_param_cb(c5_pon_borrow_info, &c5_pon_borrow_info_ops, NULL, 0400);
