#!/usr/bin/env python3
"""Adapt the ACK android15-6.6 files to the 6.18 kernel API (idempotent). Run after port-ack.py.

  xt_quota2.c       strlcpy (removed in 6.8) -> strscpy, same arguments
  ashmem.c          drop ACK's page_size_compat.h (16K page emulation); __PAGE_ALIGN -> PAGE_ALIGN
  uid_sys_stats.c   ACK's task-exit profile notifier -> the sched_process_exit tracepoint
                    (probes run with preemption off: the deferred-work allocation becomes GFP_ATOMIC);
                    ACK's per-task fsync counter (ioac.syscfs) does not exist upstream: reported as 0
  cgroup-v1.c       ACK lets a CAP_SYS_NICE holder move other users' tasks between cgroup v1 groups;
                    without it system_server cannot put apps into the top-app/foreground/background
                    cpusets ("AddTidToCgroup failed ... Permission denied") and Android's scheduling
                    policy for apps does nothing
"""
import os
import sys

T = os.path.join(sys.argv[1], 'wsl-6.18')


def edit(rel, pairs):
    p = os.path.join(T, rel)
    s = open(p).read()
    for old, new in pairs:
        if old not in s and (new == '' or new in s):
            continue  # applied by an earlier run
        assert old in s, (rel, old)
        s = s.replace(old, new)
    open(p, 'w').write(s)
    print(rel, 'ok')


edit('net/netfilter/xt_quota2.c', [('strlcpy(', 'strscpy(')])

edit('drivers/staging/android/ashmem.c', [
    ('#include <linux/page_size_compat.h>\n', ''),
    ('__PAGE_ALIGN(asma->size)', 'PAGE_ALIGN(asma->size)'),
    # mm->get_unmapped_area went away in 6.10; mm_get_unmapped_area() takes the same arguments
    ('	return current->mm->get_unmapped_area(file, addr, len, pgoff, flags);',
     '	return mm_get_unmapped_area(current->mm, file, addr, len, pgoff, flags);'),
    # static shrinkers went away in 6.7: shrinker_alloc() + shrinker_register()
    ('static struct shrinker ashmem_shrinker = {\n'
     '	.count_objects = ashmem_shrink_count,\n'
     '	.scan_objects = ashmem_shrink_scan,\n'
     '	/*\n'
     '	 * XXX (dchinner): I wish people would comment on why they need on\n'
     '	 * significant changes to the default value here\n'
     '	 */\n'
     '	.seeks = DEFAULT_SEEKS * 4,\n'
     '};\n',
     'static struct shrinker *ashmem_shrinker;\n'),
    ('			ret = ashmem_shrink_count(&ashmem_shrinker, &sc);\n'
     '			ashmem_shrink_scan(&ashmem_shrinker, &sc);\n',
     '			ret = ashmem_shrink_count(ashmem_shrinker, &sc);\n'
     '			ashmem_shrink_scan(ashmem_shrinker, &sc);\n'),
    ('	ret = register_shrinker(&ashmem_shrinker, "android-ashmem");\n'
     '	if (ret) {\n'
     '		pr_err("failed to register shrinker!\\n");\n'
     '		goto out_demisc;\n'
     '	}\n',
     '	ashmem_shrinker = shrinker_alloc(0, "android-ashmem");\n'
     '	if (!ashmem_shrinker) {\n'
     '		pr_err("failed to register shrinker!\\n");\n'
     '		ret = -ENOMEM;\n'
     '		goto out_demisc;\n'
     '	}\n'
     '	ashmem_shrinker->count_objects = ashmem_shrink_count;\n'
     '	ashmem_shrinker->scan_objects = ashmem_shrink_scan;\n'
     '	ashmem_shrinker->seeks = DEFAULT_SEEKS * 4;\n'
     '	shrinker_register(ashmem_shrinker);\n'),
])

edit('drivers/misc/uid_sys_stats.c', [
    ('#include <linux/profile.h>\n', '#include <trace/events/sched.h>\n'),
    ('	io_slot->fsync += ioac->syscfs;\n',
     '	/* upstream task_io_accounting has no fsync counter (ACK\'s syscfs) */\n'),
    ('				io.fsync += t->ioac.syscfs;\n', ''),
    ('		usw = kmalloc(sizeof(struct update_stats_work), GFP_KERNEL);',
     '		usw = kmalloc(sizeof(struct update_stats_work), GFP_ATOMIC);'),
    ('static struct notifier_block process_notifier_block = {\n	.notifier_call	= process_notifier,\n};\n',
     '/* ACK hooks task exit with profile_event_register(PROFILE_TASK_EXIT); upstream has the\n'
     ' * sched_process_exit tracepoint in the same do_exit() path, while task->ioac is still valid. */\n'
     'static void uid_sys_stats_task_exit(void *data, struct task_struct *task, bool group_dead)\n'
     '{\n'
     '	process_notifier(NULL, 0, task);\n'
     '}\n'),
    ('	profile_event_register(PROFILE_TASK_EXIT, &process_notifier_block);\n',
     '	register_trace_sched_process_exit(uid_sys_stats_task_exit, NULL);\n'),
])

# ACK behaviour on upstream code (no config switch): the same check as ACK 5.15 / 6.6 / 6.12
edit('kernel/cgroup/cgroup-v1.c', [
    ('	    !uid_eq(cred->euid, tcred->suid))\n		ret = -EACCES;',
     '	    !uid_eq(cred->euid, tcred->suid) &&\n'
     '	    !ns_capable(tcred->user_ns, CAP_SYS_NICE))\n		ret = -EACCES;'),
])
