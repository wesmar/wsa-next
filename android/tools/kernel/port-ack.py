#!/usr/bin/env python3
"""Put the Android common kernel features 6.18 lacks into the wsa-next tree (idempotent).

Run inside WSL:  python3 port-ack.py /mnt/wsl/wsa/WSA/kernel
Sources: <K>/ack-ports-6.6 (files from AOSP kernel/common android15-6.6).
  ashmem          drivers/staging/android/  (CONFIG_ASHMEM)
  xt_quota2       net/netfilter/            (CONFIG_NETFILTER_XT_MATCH_QUOTA2[_LOG])
  uid_sys_stats   drivers/misc/             (CONFIG_UID_SYS_STATS)
"""
import os
import shutil
import sys

K = sys.argv[1]
T = os.path.join(K, 'wsl-6.18')
S = os.path.join(K, 'ack-ports-6.6')

COPIES = {
    'drivers_staging_android_ashmem.c': 'drivers/staging/android/ashmem.c',
    'drivers_staging_android_ashmem.h': 'drivers/staging/android/ashmem.h',
    'drivers_staging_android_uapi_ashmem.h': 'drivers/staging/android/uapi/ashmem.h',
    'drivers_staging_android_Kconfig': 'drivers/staging/android/Kconfig',
    'drivers_staging_android_Makefile': 'drivers/staging/android/Makefile',
    'net_netfilter_xt_quota2.c': 'net/netfilter/xt_quota2.c',
    'include_linux_netfilter_xt_quota2.h': 'include/linux/netfilter/xt_quota2.h',
    'drivers_misc_uid_sys_stats.c': 'drivers/misc/uid_sys_stats.c',
}
for src, dst in COPIES.items():
    d = os.path.join(T, dst)
    os.makedirs(os.path.dirname(d), exist_ok=True)
    shutil.copyfile(os.path.join(S, src), d)


def insert(rel, anchor, text, before=True):
    p = os.path.join(T, rel)
    s = open(p).read()
    if text.strip().splitlines()[0] in s:
        return f'{rel}: already there'
    assert s.count(anchor) == 1, (rel, anchor, s.count(anchor))
    s = s.replace(anchor, text + anchor if before else anchor + text)
    open(p, 'w').write(s)
    return f'{rel}: inserted'


QUOTA2_KCONFIG = '''config NETFILTER_XT_MATCH_QUOTA2
	tristate '"quota2" match support'
	depends on NETFILTER_ADVANCED
	help
	  This option adds a `quota2' match, which allows to match on a
	  byte counter correctly and not per CPU.
	  It allows naming the quotas.
	  This is based on http://xtables-addons.git.sourceforge.net

	  If you want to compile it as a module, say M here and read
	  <file:Documentation/kbuild/modules.txt>.  If unsure, say `N'.

config NETFILTER_XT_MATCH_QUOTA2_LOG
	bool '"quota2" Netfilter LOG support'
	depends on NETFILTER_XT_MATCH_QUOTA2
	default n
	help
	  This option allows `quota2' to log ONCE when a quota limit
	  is passed. It logs via NETLINK using the NETLINK_NFLOG family.
	  It logs similarly to how ipt_ULOG would without data.

	  If unsure, say `N'.

'''
UID_KCONFIG = '''config UID_SYS_STATS
	bool "Per-UID statistics"
	depends on PROFILING && TASK_XACCT && TASK_IO_ACCOUNTING
	help
	  Per UID based cpu time statistics exported to /proc/uid_cputime
	  Per UID based io statistics exported to /proc/uid_io
	  Per UID based procstat control in /proc/uid_procstat

'''
print(insert('net/netfilter/Kconfig', 'config NETFILTER_XT_MATCH_RATEEST\n', QUOTA2_KCONFIG))
print(insert('net/netfilter/Makefile', 'obj-$(CONFIG_NETFILTER_XT_MATCH_QUOTA) += xt_quota.o\n',
             'obj-$(CONFIG_NETFILTER_XT_MATCH_QUOTA2) += xt_quota2.o\n', before=False))
print(insert('drivers/staging/Kconfig', 'source "drivers/staging/rtl8723bs/Kconfig"\n',
             'source "drivers/staging/android/Kconfig"\n\n'))
print(insert('drivers/staging/Makefile', 'obj-y				+= media/\n', 'obj-y				+= android/\n', before=False))
print(insert('drivers/misc/Kconfig', 'source "drivers/misc/keba/Kconfig"\n', UID_KCONFIG))
print(insert('drivers/misc/Makefile', 'obj-y				+= keba/\n',
             'obj-$(CONFIG_UID_SYS_STATS)	+= uid_sys_stats.o\n'))
