#!/usr/bin/env python3
"""HOST-only translation of observed C5 devfreq device names in power init.

Only names change. Existing governor requests and tuning values are retained.
Never run this transformer on recovery's similarly named power-init copy.
"""
import hashlib

PATHS = {
    '/sys/class/devfreq/qcom,mincpubw.53/': ('/sys/class/devfreq/soc:qcom,mincpubw/', 1),
    '/sys/class/devfreq/qcom,cpubw.52/': ('/sys/class/devfreq/0.qcom,cpubw/', 3),
    '/sys/class/devfreq/qcom,gpubw.39/': ('/sys/class/devfreq/soc:qcom,gpubw/', 2),
}


def transform(before):
    text = before.decode('utf-8')
    for old, (new, count) in PATHS.items():
        if text.count(old) != count or new in text:
            raise ValueError('Exact original runtime power-init path set required: '+old)
        if sum(line.lstrip().startswith('write '+old) for line in text.splitlines()) != count:
            raise ValueError('Only existing native write actions may be translated')
    for old, (new, _) in PATHS.items():
        text = text.replace(old, new)
    return text.encode('utf-8')


def review(before, after):
    if transform(before) != after:
        raise ValueError('Only exact observed device-name substitutions allowed')
    restored = after.decode('utf-8')
    for old, (new, _) in PATHS.items():
        restored = restored.replace(new, old)
    if restored.encode('utf-8') != before:
        raise ValueError('Power-init source reversal differs')
    return dict(before_sha256=hashlib.sha256(before).hexdigest(),
                after_sha256=hashlib.sha256(after).hexdigest(),
                translated_paths=PATHS, governor_requests_and_values_preserved=True,
                no_recovery_or_thermal_or_charging_change=True, device_commands=[], hardware_PASS=False)
