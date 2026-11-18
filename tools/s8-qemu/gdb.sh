#!/bin/bash
# Attach gdb to a running s8-qemu started with run.sh --gdb (or --gdb-wait), with kernel symbols.
#   gdb.sh [-p PORT] [-x extra-gdb-commands] [-- gdb args]
# Env: KERNEL (kernel Mach-O; default the CI build), GDB (default gdb-multiarch)
set -u
KERNEL=${KERNEL:-/mnt/c/github/ios-s8/upstream/xnu/out-v100/xnu-kernel/kernel.development.a5xelte}
GDB=${GDB:-gdb-multiarch}
PORT=1234; EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    -p) PORT=${2:?}; shift ;;
    -x) EXTRA=${2:?}; shift ;;
    --) shift; break ;;
    *) break ;;
  esac
  shift
done
command -v "$GDB" >/dev/null || { echo "MISSING: $GDB (apt install gdb-multiarch)"; exit 1; }
[ -f "$KERNEL" ] || echo "note: kernel $KERNEL not found: no symbols"
CMDS=$(mktemp)
{
  echo "set architecture aarch64"
  echo "set pagination off"
  echo "set confirm off"
  [ -f "$KERNEL" ] && echo "symbol-file $KERNEL"
  echo "target remote :$PORT"
  echo "define s8regs"
  echo "  info registers pc sp x0 x1 x2 x30"
  echo "end"
  [ -n "$EXTRA" ] && echo "source $EXTRA"
} > "$CMDS"
echo "Tips: 'monitor info registers' no; use QMP: devices/qmp.py hmp 'info cpus'. Kernel runs at 0xfffffff0..., symbols"
echo "      are linked there; physical-memory dump: 'monitor xp /8x 0xcc000000'. Command file: $CMDS"
exec "$GDB" -q -x "$CMDS" "$@"
