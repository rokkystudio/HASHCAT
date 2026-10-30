#!/system/bin/sh
# Execute one selected hashcat self-test vector on RedMagic using an isolated runtime.
# Read stock app assets through symlinks, but never replace application files.
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
[ "$(getprop ro.product.model)" = 'NX709S' ] || { echo 'REFUSED: not RedMagic NX709S'; exit 12; }
APP=/data/user/0/fuck.system.hashcat
R="$APP/files/hashcat/runtime"
MODE5=$(printf '%05d' "$MODE")
INPUT=/data/local/tmp/hc-redmagic-selected-input
STAGE="$APP/cache/hc-selected-$MODE5-$KERNEL-20261030"
PERSIST="$APP/cache/hc-selected-$MODE5-$KERNEL-20261030"
if pidof hashcat >/dev/null 2>&1; then echo 'REFUSED: hashcat already running'; exit 13; fi
if [ -e "$STAGE" ] || [ -e "$PERSIST.log" ] || [ -e "$PERSIST.exit" ] || [ -e "$PERSIST.recovered" ]; then echo 'REFUSED: stale stage/result exists'; exit 14; fi
if [ -e "$APP/cache/module_22000.safe.bak" ] || [ -e "$APP/cache/m22000-pure.safe.bak" ]; then echo 'REFUSED: pending mode-22000 recovery'; exit 15; fi
for f in "$INPUT/hashcat" "$INPUT/module_$MODE5.so" "$INPUT/m$MODE5.hash"; do
  [ -s "$f" ] || { echo "REFUSED: missing $f"; exit 16; }
done
mkdir -p "$STAGE/modules" "$STAGE/OpenCL" "$STAGE/_cache" "$STAGE/_data" "$STAGE/kernels" "$STAGE/hashcat.outfiles"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT HUP INT TERM
for path in "$R"/*; do
  name=${path##*/}
  case "$name" in OpenCL|modules|hashcat|hc-safe-22000|kernels|benchmark.pid|hashcat.outfiles) continue ;; esac
  ln -s "$path" "$STAGE/$name"
done
for path in "$R/modules"/*; do
  name=${path##*/}
  [ "$name" = "module_$MODE5.so" ] && continue
  ln -s "$path" "$STAGE/modules/$name"
done
for path in "$R/OpenCL"/*; do
  name=${path##*/}
  case "$name" in "m$MODE5"*) continue ;; esac
  ln -s "$path" "$STAGE/OpenCL/$name"
done
cp "$INPUT/hashcat" "$STAGE/hashcat"
chmod 700 "$STAGE/hashcat"
cp "$INPUT/module_$MODE5.so" "$STAGE/modules/module_$MODE5.so"
count=0
for path in "$INPUT/OpenCL"/*; do
  [ -f "$path" ] || continue
  name=${path##*/}
  case "$name" in "m$MODE5"*) cp "$path" "$STAGE/OpenCL/$name" ;; *) echo "REFUSED: wrong source $name"; exit 17 ;; esac
  count=$((count + 1))
done
[ "$count" -gt 0 ] || { echo 'REFUSED: no mode OpenCL source'; exit 18; }
export LD_LIBRARY_PATH="$STAGE:$R:/vendor/lib64:/system/lib64"
export HASHCAT_SHARED_FOLDER="$STAGE"
export HOME="$STAGE"
export TMPDIR="$STAGE/_cache"
export XDG_CACHE_HOME="$STAGE/_cache"
export XDG_DATA_HOME="$STAGE/_data"
cd "$STAGE/_cache"
LOG="$STAGE/result.log"
OUT="$STAGE/cracked.txt"
set +e
timeout 200 "$STAGE/hashcat" $KERNEL_ARGS -m "$MODE" -a 3 -w 1 --potfile-disable --logfile-disable --restore-disable --backend-ignore-cuda --backend-ignore-hip --outfile-format 2 -o "$OUT" "$INPUT/m$MODE5.hash" "$PASSWORD" > "$LOG" 2>&1
code=$?
set -e
printf '%s
' "$code" > "$STAGE/result.exit"
cp "$LOG" "$PERSIST.log"
cp "$STAGE/result.exit" "$PERSIST.exit"
if [ -s "$OUT" ]; then cp "$OUT" "$PERSIST.recovered"; fi
if [ "$code" -eq 0 ] && [ -s "$OUT" ] && grep -Fxq "$PASSWORD" "$OUT" && grep -Fq 'Status...........: Cracked' "$LOG" && grep -Fq 'Recovered........: 1/1 (100.00%)' "$LOG"; then
  echo "REDMAGIC_SELECTED_PASS mode=$MODE kernel=$KERNEL exit=$code"
  cat "$OUT"
  exit 0
fi
echo "REDMAGIC_SELECTED_FAIL mode=$MODE kernel=$KERNEL exit=$code"
tail -n 24 "$LOG"
exit 1
