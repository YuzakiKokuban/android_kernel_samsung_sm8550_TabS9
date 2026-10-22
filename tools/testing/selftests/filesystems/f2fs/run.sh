#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Both arguments must be dedicated, disposable, already-mounted filesystems.
# No formatting is performed here. Run in an isolated VM with fresh images.
set -eu

if [ "${F2FS_COMPAT_TEST:-0}" != 1 ]; then
	echo 'SKIP: set F2FS_COMPAT_TEST=1 and pass legacy/native scratch mountpoints'
	exit 4
fi
if [ "$#" != 2 ] || [ "$(id -u)" != 0 ]; then
	echo "usage (root): F2FS_COMPAT_TEST=1 $0 LEGACY_MOUNT NATIVE_MOUNT" >&2
	exit 2
fi

legacy=$(readlink -f "$1")
native=$(readlink -f "$2")
test "$legacy" != "$native"
# Restrict operations to two distinct F2FS mount roots, never subdirectories.
for target in "$legacy" "$native"; do
	awk -v target="$target" '$2 == target && $3 == "f2fs" { found = 1 }
		END { exit !found }' /proc/mounts
done
test "$(stat -c %d "$legacy")" != "$(stat -c %d "$native")"
test ! -e "$legacy/f2fs-compat-test"
test ! -e "$native/f2fs-compat-test"
binary=$(dirname "$(readlink -f "$0")")/project_quota_compat
mkdir "$legacy/f2fs-compat-test" "$native/f2fs-compat-test"
mkdir "$legacy/f2fs-compat-test/strict" "$legacy/f2fs-compat-test/compat"
mkdir "$native/f2fs-compat-test/native"

mount -o remount,nolegacy_project_quota "$legacy"
"$binary" "$legacy/f2fs-compat-test/strict" strict
mount -o remount,legacy_project_quota "$native"
"$binary" "$native/f2fs-compat-test/native" native
"$binary" "$legacy/f2fs-compat-test/strict" strict
mount -o remount,legacy_project_quota "$legacy"
"$binary" "$legacy/f2fs-compat-test/compat" compat
mount -o remount,ro "$legacy"
"$binary" "$legacy/f2fs-compat-test/compat" readonly
mount -o remount,rw "$legacy"
"$binary" "$legacy/f2fs-compat-test/compat" verify-compat
mount -o remount,nolegacy_project_quota "$legacy"
"$binary" "$legacy/f2fs-compat-test/strict" strict
"$binary" "$legacy" race-failed
"$binary" "$legacy" race
mount -o remount,ro "$native"
"$binary" "$native/f2fs-compat-test/native" readonly
mount -o remount,rw "$native"
"$binary" "$native/f2fs-compat-test/native" verify-native
echo 'PASS: F2FS project quota compatibility matrix'
