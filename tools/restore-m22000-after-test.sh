#!/system/bin/sh
# Restore files saved by the temporary m22000 benchmark on RedMagic.
# Run as: adb -s 91809037 shell run-as fuck.system.hashcat sh <script>
set -eu

APP=/data/user/0/fuck.system.hashcat
R="$APP/files/hashcat/runtime"
C="$APP/cache"

if pidof hc-safe-22000 >/dev/null 2>&1; then
  echo "RESTORE_REFUSED: benchmark process is still active"
  exit 20
fi

MODULE_BACKUP="$C/module_22000.safe.bak"
SOURCE_BACKUP="$C/m22000-pure.safe.bak"
MODULE="$R/modules/module_22000.so"
SOURCE="$R/OpenCL/m22000-pure.cl"

if [ ! -s "$MODULE_BACKUP" ]; then
  echo "RESTORE_REFUSED: missing module backup"
  exit 21
fi

if [ ! -s "$SOURCE_BACKUP" ]; then
  echo "RESTORE_REFUSED: missing OpenCL source backup"
  exit 22
fi

# These exact backup sizes were observed after the interrupted RedMagic test.
# Refuse a different snapshot rather than silently installing unknown files.
module_bytes=$(wc -c < "$MODULE_BACKUP")
source_bytes=$(wc -c < "$SOURCE_BACKUP")
if [ "$module_bytes" -ne 1061768 ] || [ "$source_bytes" -ne 11524 ]; then
  echo "RESTORE_REFUSED: unexpected backup size ($module_bytes/$source_bytes); backups preserved"
  exit 25
fi

# Stage verified copies in the destination directories, then rename into place.
# This prevents a failed cp from truncating either live application file.
MODULE_TMP="$MODULE.restore-tmp"
SOURCE_TMP="$SOURCE.restore-tmp"
trap 'rm -f "$MODULE_TMP" "$SOURCE_TMP"' EXIT HUP INT TERM
cp "$MODULE_BACKUP" "$MODULE_TMP"
cp "$SOURCE_BACKUP" "$SOURCE_TMP"

if ! cmp -s "$MODULE_BACKUP" "$MODULE_TMP" || ! cmp -s "$SOURCE_BACKUP" "$SOURCE_TMP"; then
  echo "RESTORE_FAILED: temporary copy mismatch; backups preserved"
  exit 26
fi

mv -f "$MODULE_TMP" "$MODULE"
mv -f "$SOURCE_TMP" "$SOURCE"

# Verify copies before deleting backups; a failed check preserves both.
set -- $(sha256sum "$MODULE_BACKUP")
backup_module_hash=$1
set -- $(sha256sum "$MODULE")
restored_module_hash=$1
set -- $(sha256sum "$SOURCE_BACKUP")
backup_source_hash=$1
set -- $(sha256sum "$SOURCE")
restored_source_hash=$1

if [ "$backup_module_hash" != "$restored_module_hash" ]; then
  echo "RESTORE_FAILED: module hash mismatch; backups preserved"
  exit 23
fi

if [ "$backup_source_hash" != "$restored_source_hash" ]; then
  echo "RESTORE_FAILED: OpenCL source hash mismatch; backups preserved"
  exit 24
fi

rm -f "$R/hc-safe-22000"
rm -f "$MODULE_BACKUP" "$SOURCE_BACKUP"

echo "RESTORE_VERIFIED: original module and OpenCL source restored"
echo "module_sha256=$restored_module_hash"
echo "source_sha256=$restored_source_hash"
