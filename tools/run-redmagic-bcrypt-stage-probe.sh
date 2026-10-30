#!/system/bin/sh
# Run one isolated GPU known-vector bcrypt probe without executing hashcat.
# Preserve all readback files and the compiler log in the app test cache.
set -eu

[ "$(getprop ro.product.model)" = 'NX709S' ] || { echo 'REFUSED: wrong model'; exit 12; }

APP=/data/user/0/fuck.system.hashcat
INPUT=/data/local/tmp/hc-bcrypt-stage-probe-20261030
STAGE="$APP/cache/hc-bcrypt-stage-probe-20261030"

[ -f "$INPUT/probe.cl" ] || { echo 'REFUSED: probe source missing'; exit 13; }
[ -x "$STAGE/probe" ] || { echo 'REFUSED: probe executable missing'; exit 14; }
[ ! -e "$STAGE/probe.log" ] || { echo 'REFUSED: existing probe log'; exit 15; }
[ ! -e "$STAGE/probe.exit" ] || { echo 'REFUSED: existing probe exit status'; exit 16; }

if pidof hashcat >/dev/null 2>&1; then
  echo 'REFUSED: application GPU process already running'
  exit 17
fi

cd "$STAGE"

set +e
timeout 180 "$STAGE/probe" "$INPUT/probe.cl" "$STAGE" hashcat > "$STAGE/probe.log" 2>&1
code=$?
set -e

printf '%s\n' "$code" > "$STAGE/probe.exit"

echo "BCRYPT_STAGE_PROBE_EXIT=$code"
tail -n 35 "$STAGE/probe.log"

if [ "$code" -eq 0 ]; then
  for stage in init loop1 loop_final comp; do
    [ -s "$STAGE/m03200-$stage.bin" ] || { echo "MISSING_STAGE=$stage"; exit 18; }
  done
fi

exit "$code"
