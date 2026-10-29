#!/system/bin/sh
# Benchmark selected hashcat modes using app-private cache without changing runtime.
# All input binaries must already be staged in /data/local/tmp by the host.
set -eu

MODE=${1:-}
PROFILE=${2:-0}
case "$PROFILE" in 0|1|2|3|4) ;; *) echo "ISOLATED_REFUSED: bad workload profile"; exit 18 ;; esac
case "$MODE" in
  0|100|1000|1400|1700|1800|22000) ;;
  *) echo "ISOLATED_REFUSED: unsupported mode: $MODE"; exit 10 ;;
esac
MODE5=$(printf '%05d' "$MODE")
APP=/data/user/0/fuck.system.hashcat
R="$APP/files/hashcat/runtime"
STAGE="$APP/cache/hc-mode-isolated-$MODE5"
INPUT="/data/local/tmp/hc-modebench-$MODE5"
BIN="$INPUT/hashcat"
MOD="$INPUT/module_$MODE5.so"

for f in "$BIN" "$MOD"; do
  if [ ! -s "$f" ]; then
    echo "ISOLATED_REFUSED: missing staged input: $f"
    exit 11
  fi
done
if [ -e "$APP/cache/module_22000.safe.bak" ] || [ -e "$APP/cache/m22000-pure.safe.bak" ]; then
  echo "ISOLATED_REFUSED: previous restoration backups remain"
  exit 13
fi
if [ -e "$STAGE" ] || [ -L "$STAGE" ]; then
  echo "ISOLATED_REFUSED: stale stage exists; inspect before removing: $STAGE"
  exit 12
fi

# Refuse simultaneous hashcat jobs; an atomic lock also prevents races between modes.
LOCK="$APP/cache/hc-mode-isolated.lock"
if pidof hashcat >/dev/null 2>&1 || pidof hc-safe-22000 >/dev/null 2>&1; then
  echo "ISOLATED_REFUSED: another hashcat test is running"
  exit 16
fi
if ! mkdir "$LOCK" 2>/dev/null; then
  echo "ISOLATED_REFUSED: benchmark lock is present; inspect before retrying"
  exit 17
fi
cleanup_stage() {
  rm -rf "$STAGE"
  rmdir "$LOCK" 2>/dev/null || true
}
trap cleanup_stage EXIT HUP INT TERM

# Avoid overwriting originals or the recovery backups under all circumstances.
mkdir -p "$STAGE/modules" "$STAGE/OpenCL"

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
  case "$name" in
    "m$MODE5"*) continue ;;
  esac
  ln -s "$path" "$STAGE/OpenCL/$name"
done
# Hashcat loads module_00000 and possibly other modules during initialization.
# Symlink those from the stock runtime; use only the experimental 22000 module.
for path in "$R"/modules/*; do
  name=${path##*/}
  if [ "$name" = "module_$MODE5.so" ]; then continue; fi
  ln -s "$path" "$STAGE/modules/$name"
done
cp "$BIN" "$STAGE/hashcat"
cp "$MOD" "$STAGE/modules/module_$MODE5.so"
source_count=0
for path in "$INPUT"/OpenCL/*; do
  [ -f "$path" ] || continue
  name=${path##*/}
  case "$name" in
    "m$MODE5"*) cp "$path" "$STAGE/OpenCL/$name" ;;
    *) echo "ISOLATED_REFUSED: unexpected input source: $name"; exit 14 ;;
  esac
  source_count=$((source_count + 1))
done
if [ "$source_count" -lt 1 ]; then
  echo "ISOLATED_REFUSED: no mode-specific OpenCL sources staged"
  exit 15
fi
chmod 700 "$STAGE/hashcat"

echo "ISOLATED_STAGE_READY mode=$MODE sources=$source_count"
sha256sum "$STAGE/hashcat" "$STAGE/modules/module_$MODE5.so"

export LD_LIBRARY_PATH="$STAGE:$R:/vendor/lib64:/system/lib64"
export HOME="$APP/files/hashcat/data"
export TMPDIR="$STAGE"
export XDG_CACHE_HOME="$STAGE/_cache"
export XDG_DATA_HOME="$STAGE/_data"
export HASHCAT_SHARED_FOLDER="$STAGE"
mkdir -p "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
cd "$STAGE"

SUFFIX=""
WORKLOAD_ARG=""
if [ "$PROFILE" -gt 0 ]; then SUFFIX="-w$PROFILE"; WORKLOAD_ARG="-w $PROFILE"; fi
RESULT_DIR="$APP/cache/hc-mode-results"
mkdir -p "$RESULT_DIR"
LOG="$RESULT_DIR/m$MODE5$SUFFIX.log"
RESULT="$RESULT_DIR/m$MODE5$SUFFIX.exit"
[ ! -e "$LOG" ] && [ ! -e "$RESULT" ] || { echo "ISOLATED_REFUSED: prior result exists"; exit 19; }
set +e
timeout 210 ./hashcat -b -m "$MODE" --runtime=3 --backend-ignore-cuda --backend-ignore-hip --logfile-disable --potfile-disable $WORKLOAD_ARG > "$LOG" 2>&1
result=$?
set -e
printf 'ISOLATED_BENCHMARK_EXIT:%s mode=%s workload=%s\n' "$result" "$MODE" "$PROFILE" >> "$LOG"
printf '%s\n' "$result" > "$RESULT.tmp"
mv -f "$RESULT.tmp" "$RESULT"
exit "$result"
