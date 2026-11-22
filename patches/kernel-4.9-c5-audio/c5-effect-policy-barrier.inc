/* SPDX-License-Identifier: Apache-2.0 */
/* Resolve the real service before AudioFlinger::mLock, not inside EffectChain. */
static void c5_wait_for_audio_policy_before_effect_lock()
{
    (void)AudioSystem::get_audio_policy_service();
}
