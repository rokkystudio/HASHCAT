#!/system/bin/sh
# Validate four unique WPA salts across PMKID and three EAPOL key versions.
# Stage test binaries and OpenCL sources under a private cache directory.
# Read all stock runtime assets without replacing the application files.
# Save hashcat logs, exit markers and successful recovered words outside
# the temporary runtime, including when the ADB connection drops.
# Require 4/4 recovered for the valid candidate, or 0/4 for the invalid one.
set -eu
MODE=22000
CASE=${1:-}
NEGATIVE=0
case "$CASE" in
  match) PASSWORD='hashcat!' ;;
  wrong) PASSWORD=hashcat2; NEGATIVE=1 ;;
  *) echo 'REFUSED: expected match or wrong'; exit 10 ;;
esac
[ "$(getprop ro.product.model)" = 'NX709S' ] || { echo 'REFUSED: not RedMagic NX709S'; exit 11; }
APP=/data/user/0/fuck.system.hashcat
R="$APP/files/hashcat/runtime"
MODE5=$(printf '%05d' "$MODE")
STAGE="$APP/cache/hc-multisalt-wpa-$CASE-20261030"
LOG="$APP/cache/hc-multisalt-wpa-$CASE-20261030.log"
EXIT="$APP/cache/hc-multisalt-wpa-$CASE-20261030.exit"
RECOVERED="$APP/cache/hc-multisalt-wpa-$CASE-20261030.recovered"
INPUT="/data/local/tmp/hc-multisalt-wpa-input"
if pidof hashcat >/dev/null 2>&1; then echo 'REFUSED: hashcat already running'; exit 12; fi
if [ -e "$STAGE" ] || [ -e "$LOG" ] || [ -e "$EXIT" ] || [ -e "$RECOVERED" ]; then
  echo 'REFUSED: previous stage/log/result exists'
  exit 13
fi
if [ -e "$APP/cache/module_22000.safe.bak" ] || [ -e "$APP/cache/m22000-pure.safe.bak" ]; then echo 'REFUSED: pending app restoration'; exit 14; fi
for f in "$INPUT/hashcat" "$INPUT/module_$MODE5.so" "$INPUT/known.hash"; do [ -s "$f" ] || { echo "REFUSED: missing $f"; exit 15; }; done
[ "$(wc -l < "$INPUT/known.hash")" -eq 4 ] || { echo 'REFUSED: expected exactly 4 WPA records'; exit 21; }
[ "$(grep -c '^WPA\*01\*' "$INPUT/known.hash")" -eq 1 ] || { echo 'REFUSED: expected one PMKID'; exit 22; }
[ "$(grep -c '^WPA\*02\*' "$INPUT/known.hash")" -eq 3 ] || { echo 'REFUSED: expected three EAPOL'; exit 23; }
[ "$(cut -d '*' -f 6 "$INPUT/known.hash" | sort -u | wc -l)" -eq 4 ] || { echo 'REFUSED: expected four distinct SSIDs'; exit 24; }
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
  case "$name" in "m$MODE5"*) cp "$path" "$STAGE/OpenCL/$name" ;; *) echo "REFUSED: wrong source $name"; exit 16 ;; esac
  count=$((count+1))
done
[ "$count" -gt 0 ] || { echo 'REFUSED: no GPU source'; exit 17; }
export LD_LIBRARY_PATH="$STAGE:$R:/vendor/lib64:/system/lib64"
export HASHCAT_SHARED_FOLDER="$STAGE"
export HOME="$STAGE"
export TMPDIR="$STAGE/_cache"
export XDG_CACHE_HOME="$STAGE/_cache"
export XDG_DATA_HOME="$STAGE/_data"
# Important: a literal mask named "hashcat" must NOT be read as the executable
# file in the current directory. Execute by absolute path from an empty cwd.
cd "$STAGE/_cache" || exit 18
printf 'REDMAGIC_MULTISALT_STAGE_READY mode=%s case=%s sources=%s\n' "$MODE" "$CASE" "$count"
set +e
timeout 240 "$STAGE/hashcat" -m "$MODE" -a 3 -w 1 --potfile-disable --logfile-disable --restore-disable --backend-ignore-cuda --backend-ignore-hip --outfile-format 2 -o "$STAGE/cracked.txt" "$INPUT/known.hash" "$PASSWORD" > "$LOG" 2>&1
result=$?
set -e
printf '%s\n' "$result" > "$EXIT"
if [ "$NEGATIVE" -eq 1 ]; then
  if [ "$result" -eq 1 ] && [ ! -s "$STAGE/cracked.txt" ] && grep -Fq 'Status...........: Exhausted' "$LOG" && grep -Fq 'Recovered........: 0/4 (0.00%)' "$LOG"; then
    echo "MULTISALT_WPA_NEGATIVE_PASS mode=$MODE exit=$result"
    exit 0
  fi
else
  if [ "$result" -eq 0 ] && [ -s "$STAGE/cracked.txt" ] &&
     [ "$(wc -l < "$STAGE/cracked.txt")" -eq 4 ] &&
     [ "$(grep -Fvx "$PASSWORD" "$STAGE/cracked.txt" | wc -l)" -eq 0 ] &&
     grep -Fq 'Recovered........: 4/4 (100.00%)' "$LOG"; then
    cp "$STAGE/cracked.txt" "$RECOVERED"
    cmp -s "$STAGE/cracked.txt" "$RECOVERED"
    echo "MULTISALT_WPA_MATCH_PASS mode=$MODE case=$CASE exit=$result"
    cat "$RECOVERED"
    exit 0
  fi
fi
echo "MULTISALT_WPA_FAIL mode=$MODE case=$CASE exit=$result"
tail -n 19 "$LOG"
exit 1
