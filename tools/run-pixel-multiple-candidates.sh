#!/system/bin/sh
# Verify GPU selection of a correct synthetic password among three candidates.
# The script uses the existing Pixel hashcat test runtime read-only.
# Logs and recovered plaintext are retained for independent host verification.
set -u

[ "$(getprop ro.product.model)" = 'Pixel 9 Pro XL' ] || {
  echo 'MULTI_REFUSED: unexpected device'
  exit 10
}

BASE=/data/local/tmp/hc-pixel-hashcat-20261030
INPUT=/data/local/tmp/hc-pixel-multi-input
STAGE=/data/local/tmp/hc-pixel-multi-20261030
CANDIDATES="$INPUT/candidates.txt"
EXPECTED=benchdemo2026

if pidof hashcat >/dev/null 2>&1; then
  echo 'MULTI_REFUSED: another hashcat is running'
  exit 11
fi
if [ -e "$STAGE" ] || [ -L "$STAGE" ]; then
  echo 'MULTI_REFUSED: prior test results exist'
  exit 12
fi
[ -x "$BASE/hashcat" ] && [ -s "$CANDIDATES" ] || {
  echo 'MULTI_REFUSED: executable or candidate file absent'
  exit 13
}
for mode in 00000 00100 01000 01400; do
  [ -s "$INPUT/m$mode.hash" ] && [ -s "$BASE/modules/module_$mode.so" ] || {
    echo "MULTI_REFUSED: incomplete mode $mode input"
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
  echo "PIXEL_MULTI_START:$mode"
  timeout 125 "$BASE/hashcat" -m "$mode" -a 0 -w 1 \
    --potfile-disable --restore-disable --logfile-disable \
    --backend-ignore-cuda --backend-ignore-hip --outfile-format 2 \
    -o "$cracked" "$INPUT/$stem.hash" "$CANDIDATES" > "$log" 2>&1
  result=$?
  printf '%s\n' "$result" > "$STAGE/logs/$stem.exit"
  if [ "$result" -eq 0 ] && [ -s "$cracked" ] \
      && [ "$(wc -l < "$cracked")" -eq 1 ] \
      && grep -Fxq "$EXPECTED" "$cracked" \
      && grep -Fq 'Status...........: Cracked' "$log" \
      && grep -Fq 'Recovered........: 1/1 (100.00%)' "$log"; then
    printf 'PASS\n' > "$STAGE/logs/$stem.status"
    echo "PIXEL_MULTI_PASS:$mode exit=$result"
  else
    printf 'FAIL\n' > "$STAGE/logs/$stem.status"
    echo "PIXEL_MULTI_FAIL:$mode exit=$result"
    tail -n 16 "$log"
    failures=$((failures + 1))
  fi
done

echo "PIXEL_MULTI_FAILURES:$failures"
[ "$failures" -eq 0 ]
