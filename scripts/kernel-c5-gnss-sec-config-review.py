#!/usr/bin/env python3
"""Restore only the measured Samsung GNSS service's exact stock IPC rule."""
import hashlib

BEFORE_SHA = 'aa8f3201eba6208b0a3c9ab039aa0e6875463c9b8710397ee6fbb26913fa80b7'
STOCK_SHA = '837f03b6c59769657224072355059e97e0052bce5701dade042e17a8aaa3500d'
RULE = b'232:4294967295:1001:3004:1021:1000'
ADDITION = b'/* VSS GPS */\n' + RULE + b'\n'


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def transform(before, stock):
    if sha(before) != BEFORE_SHA or sha(stock) != STOCK_SHA:
        raise ValueError('Exact installed and extracted stock configurations required')
    if not before.endswith(b'\n') or RULE not in stock.splitlines():
        raise ValueError('Native line boundary and exact stock rule required')
    if any(line.startswith(b'232:') for line in before.splitlines()):
        raise ValueError('Do not duplicate or replace an existing service rule')
    return before + ADDITION


def review(before, after, stock):
    if after != transform(before, stock):
        raise ValueError('Only the exact missing stock service rule may be restored')
    return dict(before_sha256=sha(before), after_sha256=sha(after), stock_sha256=sha(stock),
                service_id=232, instance='all instances, as in stock',
                original_group_ids=[1001, 3004, 1021, 1000],
                existing_rules_byte_identical=True, default_rule_unchanged=True,
                kernel_security_unchanged=True, SELinux_unchanged=True,
                policy_effect='Restore stock access to Samsung VSS GPS service only',
                owner_policy_restoration_authorization_required=True,
                GNSS_functional_PASS=False)
