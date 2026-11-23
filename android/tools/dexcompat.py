#!/usr/bin/env python3
# WSA-Next © 2026 Marek Wesołowski - wesmar
"""Static API compatibility scan: Microsoft WSA code (built for Android 13) vs a newer Android.

For every target APK/JAR it reports what resolved against the Android 13 platform but no
longer resolves against Android 14:
  missing-class     referenced class is gone
  missing-method    referenced method (name + exact signature) is gone
  missing-field     referenced field (name + type) is gone
  became-private    member still exists but is now private (IllegalAccessError)
  unimplemented     concrete class lacks a new abstract method (AbstractMethodError)
  orphan-override   method overrode a platform method in 13, overrides nothing in 14
                    (callback silently never called); A14 same-name candidates are listed
  res-moved         inlined framework resource id (const 0x01xxxxxx) now names another resource
  res-missing       inlined framework resource no longer exists
It also lists "hooks": places inside the Android 13 framework that call into Microsoft code.
Those hooks are absent from a stock AOSP 14 framework.

Usage: dexcompat.py <tree_root> <out_dir> [new_version, default a14]
  <tree_root>/{a13,aNN}/{system,system_ext,product}/...  and  <tree_root>/{a13,aNN}/apex/<name>/
  Set AAPT2=/path/to/aapt2[.exe] to let the script dump framework-res resource tables itself.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dexmodel import (ACC_ABSTRACT, ACC_CONSTRUCTOR, ACC_PRIVATE, ACC_STATIC,  # noqa: E402
                      ClassDef, Dex, dex_blobs, load_classes)

# enum Classpath from packages/modules/SdkExtensions/proto/classpaths.proto
# (0 = UNKNOWN; values verified against WSA's systemserverclasspath.pb: services.jar -> 2)
BOOT, SYSTEMSERVER, DEX2OAT_BOOT, STANDALONE_SS = 1, 2, 3, 4
MS_PREFIXES = ('Lcom/microsoft/',)
POLYMORPHIC = {'Ljava/lang/invoke/MethodHandle;', 'Ljava/lang/invoke/VarHandle;'}

# Microsoft code shipped with WSA 2407 (relative to the A13 tree).
TARGETS = {
    'Settings': ('app', 'system_ext/priv-app/Settings/Settings.apk'),
    'SystemApp': ('app', 'system_ext/priv-app/SystemApp/SystemApp.apk'),
    'SystemUIApp': ('app', 'system_ext/priv-app/SystemUIApp/SystemUIApp.apk'),
    'UserApp': ('app', 'system_ext/priv-app/UserApp/UserApp.apk'),
    'HomeApp': ('app', 'product/app/HomeApp/HomeApp.apk'),
    'pdk.notifications': ('app', 'system_ext/framework/com.microsoft.windows.pdk.notifications.jar'),
    'windows-system_server': ('server', 'system/system/framework/windows-system_server.jar'),
}
TARGET_LIBS = ['system_ext/framework/com.microsoft.windows.pdk.notifications.jar']


# ---------------------------------------------------------------- classpath discovery

def _uleb(b: bytes, i: int):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7F) << s; s += 7
        if x < 0x80:
            return r, i


def parse_classpaths_pb(data: bytes) -> list[tuple[str, int]]:
    """ExportedClasspathsJars { repeated Jar jars = 1; } Jar { string path = 1; int classpath = 2; }"""
    out, i = [], 0
    while i < len(data):
        key, i = _uleb(data, i)
        if key >> 3 == 1 and key & 7 == 2:
            ln, i = _uleb(data, i)
            jar, i = data[i:i + ln], i + ln
            path, cp, j = '', 0, 0
            while j < len(jar):
                k, j = _uleb(jar, j)
                if k & 7 == 2:
                    l2, j = _uleb(jar, j)
                    if k >> 3 == 1:
                        path = jar[j:j + l2].decode()
                    j += l2
                elif k & 7 == 0:
                    v, j = _uleb(jar, j)
                    if k >> 3 == 2:
                        cp = v
                else:
                    break
            out.append((path, cp))
        else:
            break
    return out


class Tree:
    def __init__(self, root: Path, ver: str):
        self.root = root / ver
        self.ver = ver
        sysroot = self.root / 'system'
        self.sysroot = sysroot / 'system' if (sysroot / 'system').is_dir() else sysroot

    def device_to_host(self, dev: str) -> Path | None:
        if dev.startswith('/apex/'):
            name, _, rest = dev[6:].partition('/')
            base = self.root / 'apex'
            cand = base / name
            if not cand.is_dir():
                hits = [d for d in base.iterdir() if d.name.startswith(name)]
                if not hits:
                    return None
                cand = hits[0]
            return cand / rest
        if dev.startswith('/system/'):
            return self.sysroot / dev[8:]
        if dev.startswith('/system_ext/'):
            # Android 14 build keeps WSA's system_ext partition
            return self.root.parent / 'a13' / 'system_ext' / dev[12:]
        return None

    def classpaths(self) -> dict[int, list[Path]]:
        pbs = sorted((self.sysroot / 'etc' / 'classpaths').glob('*.pb'))
        pbs += sorted((self.root / 'apex').glob('*/etc/classpaths/*.pb'))
        cps: dict[int, list[Path]] = defaultdict(list)
        for pb in pbs:
            for dev, cp in parse_classpaths_pb(pb.read_bytes()):
                host = self.device_to_host(dev)
                if host and host.is_file() and host not in cps[cp]:
                    cps[cp].append(host)
        return cps

    def optional_libs(self, cps) -> list[Path]:
        used = {p for v in cps.values() for p in v}
        return [p for p in sorted((self.sysroot / 'framework').glob('*.jar')) if p not in used]


class Platform:
    def __init__(self, tree: Tree):
        cps = tree.classpaths()
        target_names = {Path(rel).name for _, rel in TARGETS.values()}
        keep = lambda jars: [j for j in jars if j.name not in target_names]  # noqa: E731
        self.boot_jars = keep(cps[BOOT])
        self.server_jars = keep(cps[SYSTEMSERVER] + cps[STANDALONE_SS])
        self.lib_jars = keep(tree.optional_libs(cps))
        boot = load_classes(self.boot_jars, with_code=False)
        libs = load_classes(self.lib_jars, with_code=False)
        server = load_classes(self.server_jars, with_code=False)
        self.app_view = {**libs, **boot}
        self.server_view = {**server, **boot}
        print(f'[{tree.ver}] boot={len(self.boot_jars)} jars/{len(boot)} classes, '
              f'server={len(self.server_jars)}/{len(server)}, libs={len(self.lib_jars)}/{len(libs)}',
              file=sys.stderr)

    def view(self, kind: str) -> dict[str, ClassDef]:
        return self.app_view if kind == 'app' else self.server_view


# ---------------------------------------------------------------- resolution

class Resolver:
    def __init__(self, own: dict[str, ClassDef], platform: dict[str, ClassDef]):
        self.own = own
        self.platform = platform

    def get(self, name: str) -> ClassDef | None:
        return self.own.get(name) or self.platform.get(name)

    def is_platform(self, cd: ClassDef) -> bool:
        return cd.name not in self.own

    def ancestry(self, name: str):
        """Yield (ClassDef|None, name) for superclass chain then all superinterfaces (BFS)."""
        chain, ifaces, seen = [], [], set()
        cur = name
        while cur and cur not in seen:
            seen.add(cur)
            cd = self.get(cur)
            chain.append((cd, cur))
            if cd is None:
                break
            ifaces.extend(cd.interfaces)
            cur = cd.superclass
        yield from chain
        queue = list(ifaces)
        while queue:
            cur = queue.pop(0)
            if cur in seen:
                continue
            seen.add(cur)
            cd = self.get(cur)
            yield cd, cur
            if cd:
                queue.extend(cd.interfaces)
        if 'Ljava/lang/Object;' not in seen:
            yield self.get('Ljava/lang/Object;'), 'Ljava/lang/Object;'

    def resolve_method(self, cls: str, sig: str):
        if cls.startswith('['):
            return 'ok', None
        start = self.get(cls)
        if start is None:
            return 'missing-class', cls
        name = sig.split('(', 1)[0]
        for cd, cname in self.ancestry(cls):
            if cd is None:
                return 'unresolvable', cname
            m = cd.methods.get(sig)
            if m is not None:
                return ('became-private' if m.access & ACC_PRIVATE else 'ok'), cd
            if cname in POLYMORPHIC and any(s.startswith(name + '(') for s in cd.methods):
                return 'ok', cd
        return 'missing-method', None

    def resolve_field(self, cls: str, fsig: str):
        start = self.get(cls)
        if start is None:
            return 'missing-class', cls
        for cd, cname in self.ancestry(cls):
            if cd is None:
                return 'unresolvable', cname
            acc = cd.fields.get(fsig)
            if acc is not None:
                return ('became-private' if acc & ACC_PRIVATE else 'ok'), cd
        return 'missing-field', None

    def resolve_type(self, desc: str):
        base = desc.lstrip('[')
        if not base.startswith('L'):
            return 'ok', None
        return ('ok', None) if self.get(base) else ('missing-class', base)

    def unimplemented(self, cd: ClassDef) -> set[str]:
        """Abstract instance methods the concrete class `cd` inherits without an implementation."""
        if cd.is_abstract:
            return set()
        concrete, abstract = set(), set()
        for anc, _ in self.ancestry(cd.name):
            if anc is None:
                return set()   # incomplete hierarchy, cannot judge
            for sig, m in anc.methods.items():
                if m.access & ACC_STATIC or sig.startswith('<'):
                    continue
                (abstract if m.access & ACC_ABSTRACT else concrete).add(sig)
        return abstract - concrete

    def platform_overridden(self, cd: ClassDef, sig: str) -> bool:
        first = True
        for anc, _ in self.ancestry(cd.name):
            if first:
                first = False
                continue
            if anc is not None and self.is_platform(anc) and sig in anc.methods:
                m = anc.methods[sig]
                if not m.access & (ACC_PRIVATE | ACC_STATIC):
                    return True
        return False

    def same_name_candidates(self, cd: ClassDef, name: str) -> list[str]:
        out = []
        first = True
        for anc, cname in self.ancestry(cd.name):
            if first:
                first = False
                continue
            if anc is not None and self.is_platform(anc):
                out += [f'{cname}->{s}' for s in anc.methods if s.split('(', 1)[0] == name]
        return sorted(set(out))


# ---------------------------------------------------------------- scan

def short(cls: str) -> str:
    return cls[1:-1].replace('/', '.') if cls.startswith('L') and cls.endswith(';') else cls


def scan_target(label: str, kind: str, path: Path, p13: Platform, pnew: Platform, lib_classes,
                res13: dict[int, str], resnew: dict[str, int]):
    own = load_classes([path], with_code=True)
    if kind == 'server':
        # copies of platform classes inside the jar are shadowed by the platform at runtime
        shadow = p13.view('server')
        own = {k: v for k, v in own.items() if k not in shadow}
    own_all = {**lib_classes, **own} if label != 'pdk.notifications' else own
    r13 = Resolver(own_all, p13.view(kind))
    rnew = Resolver(own_all, pnew.view(kind))
    issues: dict[tuple, dict] = {}

    def add(kind_, key, where, extra=None):
        it = issues.setdefault((kind_,) + key, {'kind': kind_, 'ref': key, 'sites': set(), 'extra': extra})
        it['sites'].add(where)

    for cd in own.values():
        for m in cd.methods.values():
            where = f'{short(cd.name)}.{m.name}'
            for rkind, ref in m.refs:
                if rkind == 'method':
                    if ref[0] in own_all:   # declared by own class: still may inherit from platform
                        owner = own_all[ref[0]]
                        if ref[1] in owner.methods:
                            continue
                    s13, _ = r13.resolve_method(*ref)
                    if s13 != 'ok':
                        continue
                    s14, x = rnew.resolve_method(*ref)
                    if s14 != 'ok':
                        add(s14 if s14 != 'missing-class' else 'missing-class',
                            (short(x),) if s14 == 'missing-class' else (short(ref[0]), ref[1]), where)
                elif rkind == 'field':
                    if ref[0] in own_all and ref[1] in own_all[ref[0]].fields:
                        continue
                    s13, _ = r13.resolve_field(*ref)
                    if s13 != 'ok':
                        continue
                    s14, x = rnew.resolve_field(*ref)
                    if s14 != 'ok':
                        add(s14, (short(x),) if s14 == 'missing-class' else (short(ref[0]), ref[1]), where)
                elif rkind == 'type':
                    if r13.resolve_type(ref[0])[0] == 'ok' and rnew.resolve_type(ref[0])[0] != 'ok':
                        add('missing-class', (short(ref[0].lstrip('[')),), where)
                elif rkind == 'const' and res13:
                    name = res13.get(ref[0])
                    if name is None:
                        continue            # not a framework resource id
                    new = resnew.get(name)
                    if new is None:
                        add('res-missing', (f'0x{ref[0]:08x}', name), where)
                    elif new != ref[0]:
                        add('res-moved', (f'0x{ref[0]:08x}', name, f'0x{new:08x}'), where)

        # inheritance checks
        if not cd.is_abstract:
            for sig in sorted(rnew.unimplemented(cd) - r13.unimplemented(cd)):
                add('unimplemented', (short(cd.name), sig), short(cd.name))
        for sig, m in cd.methods.items():
            if m.access & (ACC_PRIVATE | ACC_STATIC | ACC_CONSTRUCTOR) or sig.startswith('<'):
                continue
            if r13.platform_overridden(cd, sig) and not rnew.platform_overridden(cd, sig):
                cands = rnew.same_name_candidates(cd, m.name)
                add('orphan-override', (short(cd.name), sig), short(cd.name), cands)

    # superclasses/interfaces that disappeared
    for cd in own.values():
        for sup in ([cd.superclass] if cd.superclass else []) + cd.interfaces:
            if r13.get(sup) and not rnew.get(sup):
                add('missing-class', (short(sup),), f'{short(cd.name)} (extends/implements)')
    return own, issues


def scan_hooks(p13_tree: Tree, p13: Platform):
    """Calls from Android 13 framework code into com.microsoft.* (Microsoft's framework patches)."""
    hooks = defaultdict(set)
    for jar in p13.boot_jars + p13.server_jars:
        if jar.name == 'windows-system_server.jar':
            continue
        for origin, blob in dex_blobs(jar):
            for cd in Dex(blob, origin, with_code=True).classes():
                if cd.name.startswith(MS_PREFIXES):
                    hooks[f'{jar.name}: Microsoft class embedded in framework'].add(short(cd.name))
                    continue
                for m in cd.methods.values():
                    for rkind, ref in m.refs:
                        if rkind != 'const' and ref[0].lstrip('[').startswith(MS_PREFIXES):
                            callee = short(ref[0].lstrip('[')) + (('.' + ref[1]) if len(ref) > 1 else '')
                            hooks[f'{jar.name}: {short(cd.name)}.{m.name}'].add(callee)
    return hooks


def ensure_res_dump(tree: Tree, out: Path) -> Path:
    """Create fw_res_<ver>.txt with `aapt2 dump resources` when AAPT2 is set and the dump is missing."""
    dump = out / f'fw_res_{tree.ver}.txt'
    aapt2 = os.environ.get('AAPT2')
    apk = tree.sysroot / 'framework' / 'framework-res.apk'
    if dump.is_file() or not aapt2 or not apk.is_file():
        return dump
    arg = str(apk)
    if aapt2.lower().endswith('.exe'):           # Windows aapt2 called from WSL
        arg = subprocess.check_output(['wslpath', '-w', arg], text=True).strip()
    res = subprocess.run([aapt2, 'dump', 'resources', arg], capture_output=True,
                         text=True, encoding='utf-8', errors='replace')
    dump.write_text(res.stdout, encoding='utf-8')
    return dump


def load_res_dump(path: Path) -> dict[int, str]:
    """Parse `aapt2 dump resources` output: '    resource 0x01040000 string/foo [PUBLIC]'."""
    if not path.is_file():
        return {}
    rx = re.compile(r'^\s+resource (0x[0-9a-f]{8}) (\S+)')
    out = {}
    for line in path.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        m = rx.match(line)
        if m:
            out[int(m.group(1), 16)] = m.group(2)
    return out


def main():
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    new = sys.argv[3] if len(sys.argv) > 3 else 'a14'
    out.mkdir(parents=True, exist_ok=True)
    t13, tnew = Tree(root, 'a13'), Tree(root, new)
    p13, pnew = Platform(t13), Platform(tnew)
    lib_classes = load_classes([t13.root / p for p in TARGET_LIBS], with_code=False)
    res13 = load_res_dump(ensure_res_dump(t13, out))
    resnew = load_res_dump(ensure_res_dump(tnew, out))
    resnew = {name: rid for rid, name in resnew.items()}
    if not res13:
        print('resource dumps missing: run `aapt2 dump resources framework-res.apk` into '
              f'{out}/fw_res_a13.txt and fw_res_{new}.txt (or set AAPT2) to enable the res-* checks',
              file=sys.stderr)

    report, summary, js = [], [], {}
    for label, (kind, rel) in TARGETS.items():
        path = (t13.root / rel)
        if not path.is_file():
            summary.append(f'{label:24} NOT FOUND ({rel})')
            continue
        own, issues = scan_target(label, kind, path, p13, pnew, lib_classes, res13, resnew)
        by_kind = defaultdict(list)
        for it in issues.values():
            by_kind[it['kind']].append(it)
        summary.append(f'{label:24} classes={len(own):6}  ' +
                       '  '.join(f'{k}={len(v)}' for k, v in sorted(by_kind.items())))
        report.append(f'\n{"=" * 100}\n{label}  ({rel}, {kind})\n{"=" * 100}')
        js[label] = []
        for k in ('unimplemented', 'missing-class', 'missing-method', 'missing-field',
                  'became-private', 'orphan-override', 'res-missing', 'res-moved'):
            items = sorted(by_kind.get(k, []), key=lambda i: i['ref'])
            if not items:
                continue
            report.append(f'\n--- {k} ({len(items)})')
            for it in items:
                report.append('  ' + ' -> '.join(it['ref']))
                sites = sorted(it['sites'])
                report.append('      used in: ' + ', '.join(sites[:6]) + (f' (+{len(sites) - 6})' if len(sites) > 6 else ''))
                if it['extra']:
                    report.append('      A14 has: ' + '; '.join(it['extra'][:4]))
                js[label].append({'kind': k, 'ref': list(it['ref']), 'sites': sites, 'a14_candidates': it['extra']})

    hooks = scan_hooks(t13, p13)
    hook_lines = [f'{k}\n    -> ' + '\n    -> '.join(sorted(v)) for k, v in sorted(hooks.items())]

    (out / f'compat_report_{new}.txt').write_text('\n'.join(['SUMMARY', *summary, *report]) + '\n', encoding='utf-8')
    (out / f'compat_report_{new}.json').write_text(json.dumps(js, indent=1), encoding='utf-8')
    (out / 'framework_hooks_a13.txt').write_text('\n'.join(hook_lines) + '\n', encoding='utf-8')
    print('\n'.join(summary))
    print(f'framework hooks into com.microsoft.*: {len(hooks)} sites')


if __name__ == '__main__':
    main()
