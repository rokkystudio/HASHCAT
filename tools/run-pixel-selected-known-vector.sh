#!/system/bin/sh
# Execute one selected hashcat self-test vector on Pixel 9 Pro XL.
# Read the current arm64 runtime from /data/local/tmp and write all temporary
# state under a mode-specific directory. Do not install or modify an app.
set -eu

MODE=${1:-}
KERNEL=${2:-pure}

case "$MODE" in
  500|3200|5600) PASSWORD=hashcat ;;
  *) echo 'REFUSED: unsupported selected mode'; exit 10 ;;
esac

case "$KERNEL" in
  pure) KERNEL_ARGS= ;;
  optimized) KERNEL_ARGS='-O' ;;
  *) echo 'REFUSED: kernel variant must be pure or optimized'; exit 11 ;;
esac

[ "$(getprop ro.product.model)" = 'Pixel 9 Pro XL' ] || {
  echo 'REFUSED: not Pixel 9 Pro XL'
  exit 12
}

MODE5=$(printf '%05d' "$MODE")
BASE=/data/local/tmp/hc-pixel-current-20261030
INPUT=/data/local/tmp/hc-pixel-selected-input
STAGE=/data/local/tmp/hc-pixel-selected-$MODE5-$KERNEL-20261030
PERSIST=/data/local/tmp/Pixel_9_Pro_XL-m$MODE5-$KERNEL-known

if pidof hashcat >/dev/null 2>&1; then
  echo 'REFUSED: hashcat already running'
  exit 13
fi
if [ -e "$STAGE" ] || [ -L "$STAGE" ]; then
  echo 'REFUSED: stale stage exists'
  exit 14
fi
for f in "$BASE/hashcat" "$BASE/libc++_shared.so" "$BASE/modules/module_00000.so" "$BASE/modules/module_$MODE5.so" "$INPUT/m$MODE5.hash"; do
  [ -s "$f" ] || {
    echo "REFUSED: missing $f"
    exit 15
  }
done
[ -d "$BASE/OpenCL" ] || {
  echo 'REFUSED: missing OpenCL tree'
  exit 16
}

mkdir -p "$STAGE/cache" "$STAGE/data" "$STAGE/logs" "$STAGE/kernels" "$STAGE/hashcat.outfiles"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT HUP INT TERM

export LD_LIBRARY_PATH="$BASE:/vendor/lib64:/system/lib64"
export HASHCAT_SHARED_FOLDER="$BASE"
export HOME="$STAGE"
export TMPDIR="$STAGE/cache"
export XDG_CACHE_HOME="$STAGE/cache"
export XDG_DATA_HOME="$STAGE/data"

cd "$STAGE/cache"
LOG="$STAGE/logs/m$MODE5.log"
OUT="$STAGE/logs/m$MODE5.cracked"

set +e
timeout 180 "$BASE/hashcat" $KERNEL_ARGS -m "$MODE" -a 3 -w 1 \
  --potfile-disable --logfile-disable --restore-disable \
  --backend-ignore-cuda --backend-ignore-hip --outfile-format 2 \
  -o "$OUT" "$INPUT/m$MODE5.hash" "$PASSWORD" > "$LOG" 2>&1
code=$?
set -e

printf '%s\n' "$code" > "$STAGE/logs/m$MODE5.exit"
cp "$LOG" "$PERSIST.log"
cp "$STAGE/logs/m$MODE5.exit" "$PERSIST.exit"
if [ -s "$OUT" ]; then cp "$OUT" "$PERSIST.recovered"; fi

if [ "$code" -eq 0 ] \
    && [ -s "$OUT" ] \
    && grep -Fxq "$PASSWORD" "$OUT" \
    && grep -Fq 'Status...........: Cracked' "$LOG" \
    && grep -Fq 'Recovered........: 1/1 (100.00%)' "$LOG"; then
  echo "PIXEL_SELECTED_PASS mode=$MODE kernel=$KERNEL exit=$code"
  cat "$OUT"
  exit 0
fi

echo "PIXEL_SELECTED_FAIL mode=$MODE kernel=$KERNEL exit=$code"
tail -n 24 "$LOG"
exit 1