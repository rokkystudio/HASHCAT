#!/system/bin/sh
# Pixel Mali-G715 mode 22000 experiment. Never overwrite the baseline test tree or apps.
set -eu
BASE=/data/local/tmp/hc-pixel-safetest-20261029
INPUT=/data/local/tmp/hc-pixel-split-input-20261030
STAGE=/data/local/tmp/hc-pixel-split22000-20261030
if [ "$(getprop ro.product.model)" != "Pixel 9 Pro XL" ]; then echo "REFUSED: wrong device"; exit 10; fi
if [ -e "$STAGE" ] || [ -L "$STAGE" ]; then echo "REFUSED: stale split stage"; exit 11; fi
for file in "$INPUT/hashcat" "$INPUT/module_22000.so" "$INPUT/m22000-pure.cl" "$BASE/OpenCL/inc_vendor.h" "$BASE/libc++_shared.so"; do
  if [ ! -s "$file" ]; then echo "REFUSED: missing $file"; exit 12; fi
done
if [ -e "$INPUT/benchmark.log" ]; then echo "REFUSED: previous benchmark log exists"; exit 13; fi
mkdir -p "$STAGE/modules" "$STAGE/OpenCL" "$STAGE/kernels" "$INPUT"
cleanup_stage() { rm -rf "$STAGE"; }
trap cleanup_stage EXIT
for path in "$BASE"/*; do
  name=$(basename "$path")
  case "$name" in hashcat|modules|OpenCL|kernels|benchmark.pid) continue ;; esac
  ln -s "$path" "$STAGE/$name"
done
for path in "$BASE/OpenCL"/*; do
  name=$(basename "$path")
  [ "$name" = "m22000-pure.cl" ] && continue
  ln -s "$path" "$STAGE/OpenCL/$name"
done
for path in "$BASE/modules"/*; do
  name=$(basename "$path")
  [ "$name" = "module_22000.so" ] && continue
  ln -s "$path" "$STAGE/modules/$name"
done
cp "$INPUT/hashcat" "$STAGE/hashcat"
cp "$INPUT/module_22000.so" "$STAGE/modules/module_22000.so"
cp "$INPUT/m22000-pure.cl" "$STAGE/OpenCL/m22000-pure.cl"
chmod 700 "$STAGE/hashcat"
mkdir -p "$STAGE/cache" "$STAGE/data"
export LD_LIBRARY_PATH="$STAGE:/vendor/lib64:/system/lib64"
export HOME="$STAGE"
export TMPDIR="$STAGE/cache"
export XDG_CACHE_HOME="$STAGE/cache"
export XDG_DATA_HOME="$STAGE/data"
export HASHCAT_SHARED_FOLDER="$STAGE"
cd "$STAGE"
echo "PIXEL_SPLIT_STAGE_READY"
sha256sum ./hashcat ./modules/module_22000.so ./OpenCL/m22000-pure.cl
set +e
timeout 210 ./hashcat -b -m 22000 --runtime=3 --backend-ignore-cuda --backend-ignore-hip --logfile-disable --potfile-disable > "$INPUT/benchmark.log" 2>&1
ret=$?
set -e
echo "PIXEL_SPLIT_BENCHMARK_EXIT:$ret" | tee -a "$INPUT/benchmark.log"
tail -n 38 "$INPUT/benchmark.log"
exit "$ret"
