#!/system/bin/sh
# Execute synthetic SHA-512, SHA-512crypt and PMKID vectors on RedMagic GPU.
# Stage test binaries and OpenCL sources under a private cache directory.
# Read all stock runtime assets without replacing the application files.
# Save hashcat logs, exit markers and successful recovered words outside
# the temporary runtime, including when the ADB connection drops.
# For mode 22000, explicit mismatch and AUX wrong-passphrase cases require
# the wrong candidate to exhaust the search without a recovered hash.
set -eu
MODE=${1:-}
CASE=${2:-match}
NEGATIVE=0
case "$MODE" in 1700|1800) PASSWORD=hashcat ;; 22000) PASSWORD='hashcat!' ;; *) echo 'REFUSED: unsupported known-vector mode'; exit 10 ;; esac
case "$CASE" in
  match|aux1|aux2|aux3)
    if [ "$CASE" != match ] && [ "$MODE" != 22000 ]; then
      echo 'REFUSED: AUX case requires WPA mode 22000'
      exit 19
    fi
    ;;
  mismatch|aux1-wrong|aux2-wrong|aux3-wrong)
    [ "$MODE" = 22000 ] || { echo 'REFUSED: mismatch test requires mode 22000'; exit 19; }
    PASSWORD=hashcat2
    NEGATIVE=1
    ;;
  *) echo 'REFUSED: unsupported validation case'; exit 20 ;;
esac
[ "$(getprop ro.product.model)" = 'NX709S' ] || { echo 'REFUSED: not RedMagic NX709S'; exit 11; }
APP=/data/user/0/fuck.system.hashcat
R="$APP/files/hashcat/runtime"
MODE5=$(printf '%05d' "$MODE")
STAGE="$APP/cache/hc-known-$MODE5-$CASE-20261030"
LOG="$APP/cache/hc-known-$MODE5-$CASE-20261030.log"
EXIT="$APP/cache/hc-known-$MODE5-$CASE-20261030.exit"
RECOVERED="$APP/cache/hc-known-$MODE5-$CASE-20261030.recovered"
INPUT="/data/local/tmp/hc-known-$MODE5-input"
if pidof hashcat >/dev/null 2>&1; then echo 'REFUSED: hashcat already running'; exit 12; fi
if [ -e "$STAGE" ] || [ -e "$LOG" ] || [ -e "$EXIT" ] || [ -e "$RECOVERED" ]; then
  echo 'REFUSED: previous stage/log/result exists'
  exit 13
fi
if [ -e "$APP/cache/module_22000.safe.bak" ] || [ -e "$APP/cache/m22000-pure.safe.bak" ]; then echo 'REFUSED: pending app restoration'; exit 14; fi
for f in "$INPUT/hashcat" "$INPUT/module_$MODE5.so" "$INPUT/known.hash"; do [ -s "$f" ] || { echo "REFUSED: missing $f"; exit 15; }; done
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
printf 'REDMAGIC_KNOWN_STAGE_READY mode=%s case=%s sources=%s\n' "$MODE" "$CASE" "$count"
set +e
timeout 240 "$STAGE/hashcat" -m "$MODE" -a 3 -w 1 --potfile-disable --logfile-disable --restore-disable --backend-ignore-cuda --backend-ignore-hip --outfile-format 2 -o "$STAGE/cracked.txt" "$INPUT/known.hash" "$PASSWORD" > "$LOG" 2>&1
result=$?
set -e
printf '%s\n' "$result" > "$EXIT"
if [ "$NEGATIVE" -eq 1 ]; then
  if [ "$result" -eq 1 ] && [ ! -s "$STAGE/cracked.txt" ] && grep -Fq 'Status...........: Exhausted' "$LOG" && grep -Fq 'Recovered........: 0/1 (0.00%)' "$LOG"; then
    echo "KNOWN_VECTOR_NEGATIVE_PASS mode=$MODE exit=$result"
    exit 0
  fi
else
  if [ "$result" -eq 0 ] && [ -s "$STAGE/cracked.txt" ] && grep -Fxq "$PASSWORD" "$STAGE/cracked.txt"; then
    cp "$STAGE/cracked.txt" "$RECOVERED"
    cmp -s "$STAGE/cracked.txt" "$RECOVERED"
    echo "KNOWN_VECTOR_PASS mode=$MODE case=$CASE exit=$result"
    cat "$RECOVERED"
    exit 0
  fi
fi
echo "KNOWN_VECTOR_FAIL mode=$MODE exit=$result"
tail -n 19 "$LOG"
exit 1
