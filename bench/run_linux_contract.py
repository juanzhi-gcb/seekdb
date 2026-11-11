#!/usr/bin/env python3
"""Resolve a Linux filesystem partition to its cgroup-accounted block device.

The unchanged collector still rejects missing I/O counters. This wrapper only
resolves the kernel's partition/whole-device identity through sysfs.
"""
import os
from pathlib import Path
import sys
import run_contract_benchmark as benchmark


def cgroup_block_device(path):
    device = os.stat(path, follow_symlinks=False).st_dev
    number = f'{os.major(device)}:{os.minor(device)}'
    sysfs = (Path('/sys/dev/block') / number).resolve(strict=True)
    if (sysfs / 'partition').is_file():
        number = (sysfs.parent / 'dev').read_text().strip()
    major, minor = number.split(':')
    if not major.isdigit() or not minor.isdigit():
        raise ValueError('Invalid sysfs block device: ' + number)
    return number


benchmark.data_device = cgroup_block_device
if __name__ == '__main__':
    sys.exit(benchmark.main())
