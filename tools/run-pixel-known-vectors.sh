#!/system/bin/sh
# Pixel-only end-to-end synthetic hashcat checks. Never modifies an app.
set -u
[ "$(getprop ro.product.model)" = 'Pixel 9 Pro XL' ] || { echo 'REFUSED: not Pixel 9 Pro XL'; exit 10; }
BASE=/data/local/tmp/hc-pixel-hashcat-20261030
INPUT=/data/local/tmp/hc-pixel-known-vectors-input
STAGE=/data/local/tmp/hc-pixel-known-vectors-20261030
if pidof hashcat >/dev/null 2>&1; then echo 'REFUSED: hashcat already running'; exit 11; fi
if [ -e "$STAGE" ]; then echo 'REFUSED: stale output stage exists'; exit 12; fi
if [ ! -x "$BASE/hashcat" ]; then echo 'REFUSED: no staged hashcat'; exit 13; fi
for m in 00000 00100 01000 01400; do
  [ -s "$INPUT/m$m.hash" ] || { echo "REFUSED: missing m$m hash"; exit 14; }
  [ -s "$BASE/modules/module_$m.so" ] || { echo "REFUSED: missing module $m"; exit 15; }
done
mkdir -p "$STAGE/cache" "$STAGE/data" "$STAGE/logs"
export LD_LIBRARY_PATH="$BASE:/vendor/lib64:/system/lib64"
export HASHCAT_SHARED_FOLDER="$BASE"
export HOME="$STAGE"
export TMPDIR="$STAGE/cache"
export XDG_CACHE_HOME="$STAGE/cache"
export XDG_DATA_HOME="$STAGE/data"
cd "$BASE" || exit 16
failures=0
for mode in 0 100 1000 1400; do
  m=$(printf '%05d' "$mode")
  log="$STAGE/logs/m$m.log"
  outfile="$STAGE/logs/m$m.cracked"
  printf 'START_KNOWN_VECTOR mode=%s
' "$mode"
  timeout 120 ./hashcat -m "$mode" -a 3 --potfile-disable --logfile-disable --restore-disable --backend-ignore-cuda --backend-ignore-hip -w 1 -o "$outfile" "$INPUT/m$m.hash" benchdemo2026 > "$log" 2>&1
  code=$?
  printf '%s
' "$code" > "$STAGE/logs/m$m.exit"
  if [ "$code" -eq 0 ] && [ -s "$outfile" ] && grep -q ":benchdemo2026$" "$outfile"; then
    echo "KNOWN_VECTOR_PASS mode=$mode exit=$code"
    printf 'PASS
' > "$STAGE/logs/m$m.status"
  else
    echo "KNOWN_VECTOR_FAIL mode=$mode exit=$code"
    tail -n 18 "$log"
    printf 'FAIL
' > "$STAGE/logs/m$m.status"
    failures=$((failures + 1))
  fi
done
printf 'KNOWN_VECTOR_FAILURES:%s
' "$failures"
[ "$failures" -eq 0 ]
