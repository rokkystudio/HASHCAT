#!/system/bin/sh
# Run an Adreno m22000 experiment in app-private cache without changing runtime.
# All input binaries must already be staged in /data/local/tmp by the host.
set -eu

APP=/data/user/0/fuck.system.hashcat
R="$APP/files/hashcat/runtime"
STAGE="$APP/cache/hc-m22000-isolated-20261029"
BIN=/data/local/tmp/hc-isolated-22000
MOD=/data/local/tmp/module_22000.isolated.so
CL=/data/local/tmp/m22000-pure.isolated.cl

for f in "$BIN" "$MOD" "$CL"; do
  if [ ! -s "$f" ]; then
    echo "ISOLATED_REFUSED: missing staged input: $f"
    exit 11
  fi
done
if [ -e "$STAGE" ] || [ -L "$STAGE" ]; then
  echo "ISOLATED_REFUSED: stale stage exists; inspect before removing: $STAGE"
  exit 12
fi

# Avoid overwriting originals or the recovery backups under all circumstances.
mkdir -p "$STAGE/modules" "$STAGE/OpenCL"
cleanup_stage() {
  rm -rf "$STAGE"
}
trap cleanup_stage EXIT HUP INT TERM

# Link unmodified runtime assets into the isolated tree.
for path in "$R"/*; do
  name=${path##*/}
  case "$name" in
    OpenCL|modules|hashcat|hc-safe-22000) continue ;;
  esac
  ln -s "$path" "$STAGE/$name"
done
for path in "$R"/OpenCL/*; do
  name=${path##*/}
  if [ "$name" = 'm22000-pure.cl' ]; then continue; fi
  ln -s "$path" "$STAGE/OpenCL/$name"
done
# Hashcat loads module_00000 and possibly other modules during initialization.
# Symlink those from the stock runtime; use only the experimental 22000 module.
for path in "$R"/modules/*; do
  name=${path##*/}
  if [ "$name" = 'module_22000.so' ]; then continue; fi
  ln -s "$path" "$STAGE/modules/$name"
done
cp "$BIN" "$STAGE/hashcat"
cp "$MOD" "$STAGE/modules/module_22000.so"
cp "$CL" "$STAGE/OpenCL/m22000-pure.cl"
chmod 700 "$STAGE/hashcat"

echo "ISOLATED_STAGE_READY"
sha256sum "$STAGE/hashcat" "$STAGE/modules/module_22000.so" "$STAGE/OpenCL/m22000-pure.cl"

export LD_LIBRARY_PATH="$STAGE:$R:/vendor/lib64:/system/lib64"
export HOME="$APP/files/hashcat/data"
export TMPDIR="$APP/cache"
export XDG_CACHE_HOME="$APP/cache"
export XDG_DATA_HOME="$APP/files/hashcat/data"
export HASHCAT_SHARED_FOLDER="$STAGE"
cd "$STAGE"

set +e
./hashcat -b -m 22000 --runtime=3 --backend-ignore-cuda --backend-ignore-hip --logfile-disable
result=$?
set -e
echo "ISOLATED_BENCHMARK_EXIT:$result"
exit "$result"
