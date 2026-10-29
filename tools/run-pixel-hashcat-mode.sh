#!/system/bin/sh
# Bounded standalone GPU benchmark for Pixel 9 Pro XL; never enters app storage.
set -eu

MODE=${1:-}
PROFILE=${2:-0}
case "$PROFILE" in 0|1|2|3|4) ;; *) echo "PIXEL_REFUSED: bad workload profile"; exit 17 ;; esac
case "$MODE" in
  0|100|1000|1400|1700|1800|22000) ;;
  *) echo "PIXEL_REFUSED: unsupported mode"; exit 10 ;;
esac
[ "$(getprop ro.product.model)" = 'Pixel 9 Pro XL' ] || { echo 'PIXEL_REFUSED: wrong model'; exit 11; }
MODE5=$(printf '%05d' "$MODE")
SUFFIX=""
WORKLOAD_ARG=""
if [ "$PROFILE" -gt 0 ]; then SUFFIX="-w$PROFILE"; WORKLOAD_ARG="-w $PROFILE"; fi
R=/data/local/tmp/hc-pixel-hashcat-20261030
[ -s "$R/hashcat" ] && [ -s "$R/modules/module_$MODE5.so" ] || { echo 'PIXEL_REFUSED: incomplete stage'; exit 12; }
mkdir -p "$R/logs" "$R/cache" "$R/data"
# Do not compete with any foreground benchmark started by another shell.
if pidof hashcat >/dev/null 2>&1; then
  echo "PIXEL_REFUSED: a hashcat benchmark is already running"
  exit 15
fi
LOG="$R/logs/m$MODE5$SUFFIX.log"
RESULT="$R/logs/m$MODE5$SUFFIX.exit"
LOCK="$R/logs/m$MODE5$SUFFIX.lock"
if [ -e "$LOG" ] || [ -e "$RESULT" ]; then
  echo "PIXEL_REFUSED: prior benchmark result exists at $LOG"
  exit 13
fi
GLOBAL_LOCK="$R/logs/pixel-hashcat.lock"
if ! mkdir "$GLOBAL_LOCK" 2>/dev/null; then
  echo 'PIXEL_REFUSED: another Pixel mode test holds the GPU lock'
  exit 16
fi
if ! mkdir "$LOCK" 2>/dev/null; then
  rmdir "$GLOBAL_LOCK" 2>/dev/null || true
  echo 'PIXEL_REFUSED: benchmark already running'
  exit 14
fi
trap 'rmdir "$LOCK" 2>/dev/null || true; rmdir "$GLOBAL_LOCK" 2>/dev/null || true' EXIT HUP INT TERM

export LD_LIBRARY_PATH="$R:/vendor/lib64:/system/lib64"
export HASHCAT_SHARED_FOLDER="$R"
export HOME="$R"
export TMPDIR="$R/cache"
export XDG_CACHE_HOME="$R/cache"
export XDG_DATA_HOME="$R/data"
cd "$R"

# Hard wall-clock limit includes OpenCL compilation, autotuning and execution.
set +e
timeout 210 ./hashcat -b -m "$MODE" --runtime=3 --backend-ignore-cuda --backend-ignore-hip --logfile-disable --potfile-disable $WORKLOAD_ARG > "$LOG" 2>&1
RC=$?
set -e
printf 'PIXEL_BENCHMARK_EXIT:%s mode=%s workload=%s\n' "$RC" "$MODE" "$PROFILE" >> "$LOG"
printf '%s\n' "$RC" > "$RESULT.tmp"
mv -f "$RESULT.tmp" "$RESULT"
exit "$RC"
