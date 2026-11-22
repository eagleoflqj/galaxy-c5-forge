#!/usr/bin/env python3
"""Refresh the existing Recorder button on its existing saved-item preference event."""
import hashlib

PINS = {
    'RecorderActivity.java': 'b655f198ed37eaac72a4c297520ea4055f93a5359b17fea25a8b705fb9c74788',
    'utils/LastRecordHelper.java': '92c36c97859f3939d59adac0785c383024afbfb26c7cf80574aa6c14cbb7bdad',
}
ACTIVITY_BEFORE = b'''    public void onSharedPreferenceChanged(SharedPreferences prefs, String key) {
        if (Utils.KEY_RECORDING.equals(key)) {
            refresh();
        }
    }'''
ACTIVITY_AFTER = b'''    public void onSharedPreferenceChanged(SharedPreferences prefs, String key) {
        if (Utils.KEY_RECORDING.equals(key)) {
            refresh();
        } else if (LastRecordHelper.isLastItemPreference(key)) {
            updateLastItemStatus();
        }
    }'''
ANCHOR = b'    public static Uri getLastItemUri(Context context, boolean isSound) {\n'
HELPER = b'''    public static boolean isLastItemPreference(String key) {
        return KEY_LAST_SOUND.equals(key) || KEY_LAST_SCREEN.equals(key);
    }

'''


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def transform(files):
    if set(files) != set(PINS) or any(sha(files[p]) != pin for p, pin in PINS.items()):
        raise ValueError('Exact clean LOS17.1 Recorder input files required')
    activity, helper = files['RecorderActivity.java'], files['utils/LastRecordHelper.java']
    if activity.count(ACTIVITY_BEFORE) != 1 or helper.count(ANCHOR) != 1 or b'isLastItemPreference' in helper:
        raise ValueError('Exact existing preference callback/getter anchors required')
    return {'RecorderActivity.java': activity.replace(ACTIVITY_BEFORE, ACTIVITY_AFTER, 1),
            'utils/LastRecordHelper.java': helper.replace(ANCHOR, HELPER+ANCHOR, 1)}


def review(before, after):
    if after != transform(before):
        raise ValueError('Only saved-item key predicate and existing-listener UI refresh allowed')
    return dict(result='HOST_RECORDER_SAVED_ITEM_REFRESH_REVIEW_PASS_NOT_DEVICE', before_sha256={p: sha(raw) for p, raw in before.items()}, after_sha256={p: sha(raw) for p, raw in after.items()}, existing_preference_listener_reused=True, storage_or_audio_pipeline_changed=False, new_broadcasts_or_permissions=False, owner_UI_acceptance=False)
