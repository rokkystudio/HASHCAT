#!/system/bin/sh
# Validate that the Pixel GPU rejects an incorrect synthetic candidate.
# Read the hashcat executable and modules from the existing test runtime.
# Keep every result in a distinct temporary directory for host-side review.
set -u

[ "$(getprop ro.product.model)" = 'Pixel 9 Pro XL' ] || {
  echo "NEGATIVE_REFUSED: unexpected device"
  exit 10
}

BASE=/data/local/tmp/hc-pixel-hashcat-20261030
INPUT=/data/local/tmp/hc-pixel-negative-vectors-input
STAGE=/data/local/tmp/hc-pixel-negative-vectors-20261030
WRONG=benchdemo2027

if pidof hashcat >/dev/null 2>&1; then
  echo "NEGATIVE_REFUSED: hashcat is running"
  exit 11
fi
if [ -e "$STAGE" ] || [ -L "$STAGE" ]; then
  echo "NEGATIVE_REFUSED: previous results exist"
  exit 12
fi
[ -x "$BASE/hashcat" ] || {
  echo "NEGATIVE_REFUSED: staged hashcat is absent"
  exit 13
}
for mode in 00000 00100 01000 01400; do
  [ -s "$INPUT/m$mode.hash" ] && [ -s "$BASE/modules/module_$mode.so" ] || {
    echo "NEGATIVE_REFUSED: missing mode $mode assets"
    exit 14
  }
done

mkdir -p "$STAGE/logs" "$STAGE/cache" "$STAGE/data"
export LD_LIBRARY_PATH="$BASE:/vendor/lib64:/system/lib64"
export HASHCAT_SHARED_FOLDER="$BASE"
export HOME="$STAGE"
export TMPDIR="$STAGE/cache"
export XDG_CACHE_HOME="$STAGE/cache"
export XDG_DATA_HOME="$STAGE/data"
cd "$STAGE/cache" || exit 15

failures=0
for mode in 0 100 1000 1400; do
  stem=$(printf 'm%05d' "$mode")
  log="$STAGE/logs/$stem.log"
  cracked="$STAGE/logs/$stem.cracked"
  echo "PIXEL_NEGATIVE_START:$mode"
  timeout 125 "$BASE/hashcat" -m "$mode" -a 3 -w 1 \
    --potfile-disable --restore-disable --logfile-disable \
    --backend-ignore-cuda --backend-ignore-hip --outfile-format 2 \
    -o "$cracked" "$INPUT/$stem.hash" "$WRONG" > "$log" 2>&1
  code=$?
  printf '%s\n' "$code" > "$STAGE/logs/$stem.exit"
  if [ "$code" -eq 1 ] && [ ! -s "$cracked" ] \
      && grep -Fq 'Status...........: Exhausted' "$log" \
      && grep -Fq 'Recovered........: 0/1 (0.00%)' "$log" \
      && grep -Fq 'Candidate.Engine.: Device Generator' "$log" \
      && grep -Fq "Guess.Mask.......: $WRONG [13]" "$log"; then
    echo "PIXEL_NEGATIVE_PASS:$mode exit=$code"
    printf 'PASS\n' > "$STAGE/logs/$stem.status"
  else
    echo "PIXEL_NEGATIVE_FAIL:$mode exit=$code"
    tail -n 18 "$log"
    printf 'FAIL\n' > "$STAGE/logs/$stem.status"
    failures=$((failures + 1))
  fi
done

echo "PIXEL_NEGATIVE_FAILURES:$failures"
[ "$failures" -eq 0 ]
