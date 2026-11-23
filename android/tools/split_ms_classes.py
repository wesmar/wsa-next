#!/usr/bin/env python3
# WSA-Next © 2026 Marek Wesołowski - wesmar
"""Split Microsoft classes of a WSA app into "binary" and "port" sets.

binary  com.microsoft.* (and WSA glue) classes that only use the public/hidden framework.
        They keep working on a newer Android as compiled bytecode and are copied into
        <out>-binary.jar (taken from a dex2jar conversion of the same APK).
port    classes that extend or call SystemUI / WM Shell / Dagger internals. These must be
        recompiled from source against the new SystemUI-core; they are only listed.

Usage: split_ms_classes.py <app.apk> <app-dex2jar.jar> <out_prefix> [support_prefix ...]
  support_prefix  extra class-name prefixes copied as-is into the binary jar, for runtime
                  libraries the Microsoft classes need and the target app does not provide
                  (e.g. com/google/protobuf/ for protobuf-lite, windows/ for AIDL stubs).
"""
from __future__ import annotations

import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dexmodel import load_classes  # noqa: E402

MS = ('Lcom/microsoft/', 'Lcom/android/systemui/wsa/', 'Lcom/android/wm/shell/dagger/Wsa')
INTERNAL = ('Lcom/android/systemui/', 'Lcom/android/wm/shell/', 'Lcom/android/keyguard/',
            'Lcom/android/settingslib/', 'Lcom/android/launcher3/', 'Ldagger/', 'Ljavax/inject/')


def main():
    apk, d2j_jar, prefix = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    support = tuple(sys.argv[4:])
    classes = load_classes([apk], with_code=True)
    ms = {k: v for k, v in classes.items() if k.startswith(MS)}

    def coupled(cd) -> bool:
        types = {cd.superclass, *cd.interfaces} - {None}
        for m in cd.methods.values():
            types.update(ref[0].lstrip('[') for kind, ref in m.refs if kind != 'const')
        return any(t.startswith(INTERNAL) and not t.startswith(MS) for t in types)

    port = {k for k, cd in ms.items() if coupled(cd)}
    # Inner/synthetic classes follow their outer class
    outer = lambda n: n.split('$', 1)[0].rstrip(';') + ';'  # noqa: E731
    port_outer = {outer(n) for n in port}
    port |= {k for k in ms if outer(k) in port_outer}
    binary = set(ms) - port

    # dex2jar renames D8 synthetic classes "Foo-IA" to "Foo_IA" (and all references to them)
    wanted = {n[1:-1].replace('-', '_') + '.class' for n in binary}
    written = supported = 0
    with zipfile.ZipFile(d2j_jar) as zin, zipfile.ZipFile(f'{prefix}-binary.jar', 'w', zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            if info.filename in wanted:
                zout.writestr(info.filename, zin.read(info.filename))
                written += 1
            elif support and info.filename.endswith('.class') and info.filename.startswith(support):
                zout.writestr(info.filename, zin.read(info.filename))
                supported += 1
    Path(f'{prefix}-port.txt').write_text(
        '\n'.join(sorted(n[1:-1].replace('/', '.') for n in port)) + '\n', encoding='utf-8')
    missing = len(wanted) - written
    print(f'{apk.name}: microsoft={len(ms)} binary={len(binary)} (jar entries {written}, missing {missing}) '
          f'support={supported} port={len(port)} -> {prefix}-binary.jar, {prefix}-port.txt')


if __name__ == '__main__':
    main()
