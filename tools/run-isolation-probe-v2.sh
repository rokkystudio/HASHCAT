#!/system/bin/sh
APP=/data/user/0/fuck.system.hashcat
STAGE=$APP/files/hashcat/runtime/probe-allaux-20261028-v2
export LD_LIBRARY_PATH=/vendor/lib64:/system/lib64:$APP/files/hashcat/runtime
exec "$STAGE/opencl-probe" probe22000 "$STAGE/$1" noinline
