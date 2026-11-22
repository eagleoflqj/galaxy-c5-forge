/* SPDX-License-Identifier: GPL-2.0-only */
/* Explicit workspace lifetime. The native adapter supplies this CPU's slot
 * only while IRQs are off and the primary rq is locked. Pure helpers do not
 * provide synchronization or make arbitrary caller/callback memory safe.
 */
#ifndef _C5_LEGACY_WINDOW_WORKSPACE_H
#define _C5_LEGACY_WINDOW_WORKSPACE_H
#include "c5_legacy_window_migration.h"

enum c5_legacy_window_workspace_kind {
    C5_LEGACY_WORKSPACE_UPDATE=1,
    C5_LEGACY_WORKSPACE_MIGRATION=2,
};

struct c5_legacy_window_workspace {
    u64 generation;
    const void *owner;
    u32 cpu,kind;
    union {
        struct c5_legacy_window_update_scratch update;
        struct c5_legacy_window_migration_scratch migration;
    } scratch;
    union {
        struct c5_legacy_window_journal update;
        struct c5_legacy_window_migration_journal migration;
    } journal;
    struct c5_legacy_window_trace_view trace;
};

struct c5_legacy_window_lease {
    struct c5_legacy_window_workspace *workspace;
    u64 generation;
    const void *owner;
    u32 cpu,kind;
};

/* Caller initializes lease.workspace to NULL. Errors preserve both the
 * existing lease and slot, including the payload of an outer transaction.
 * Only successful acquisition invalidates old output by clearing its count.
 */
static inline int c5_legacy_window_workspace_acquire(
        struct c5_legacy_window_workspace *workspace,u32 cpu,
        const void *owner,u32 kind,struct c5_legacy_window_lease *lease)
{
    if (!workspace || !owner || !lease ||
        (kind!=C5_LEGACY_WORKSPACE_UPDATE && kind!=C5_LEGACY_WORKSPACE_MIGRATION))
        return -EINVAL;
    if (c5_legacy_window_storage_overlaps(workspace,sizeof(*workspace),lease,sizeof(*lease)))
        return -EINVAL;
    if (lease->workspace || workspace->kind || workspace->trace.active)
        return -EBUSY;
    if (workspace->generation==(u64)-1)
        return -EOVERFLOW;
    workspace->generation++;
    workspace->owner=owner;workspace->cpu=cpu;workspace->kind=kind;
    /* Both journal types put count first; use the active union member. */
    if (kind==C5_LEGACY_WORKSPACE_UPDATE)
        workspace->journal.update.count=0;
    else
        workspace->journal.migration.count=0;
    lease->workspace=workspace;lease->generation=workspace->generation;
    lease->owner=owner;lease->cpu=cpu;lease->kind=kind;
    return 0;
}

/* Consume the token only on success. Wrong/stale context cannot free a slot
 * belonging to a newer transaction. Generation is never allowed to wrap.
 */
static inline int c5_legacy_window_workspace_release(
        struct c5_legacy_window_workspace *workspace,u32 cpu,
        const void *owner,struct c5_legacy_window_lease *lease)
{
    if (!workspace || !owner || !lease ||
        c5_legacy_window_storage_overlaps(workspace,sizeof(*workspace),lease,sizeof(*lease)))
        return -EINVAL;
    if (lease->cpu!=cpu || workspace->cpu!=cpu)
        return -EXDEV;
    if (lease->workspace!=workspace || !workspace->kind ||
        lease->generation!=workspace->generation ||
        lease->owner!=owner || workspace->owner!=owner ||
        lease->kind!=workspace->kind)
        return -ESTALE;
    if (workspace->trace.active)
        return -EBUSY;
    if (lease->kind==C5_LEGACY_WORKSPACE_UPDATE)
        workspace->journal.update.count=0;
    else
        workspace->journal.migration.count=0;
    workspace->owner=NULL;workspace->kind=0;
    memset(lease,0,sizeof(*lease));
    return 0;
}
#endif
