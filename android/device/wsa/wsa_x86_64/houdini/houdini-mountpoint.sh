#!/system/bin/sh
# WSA-Next © 2026 Marek Wesołowski - wesmar
# Mount point only: WSA's vendor init bind-mounts /vendor/bin/houdini (or houdini64) over
# this file when native bridge execution is enabled. Running it means that did not happen.
echo "$0: Houdini is not mounted from /vendor/bin" >&2
exit 1
