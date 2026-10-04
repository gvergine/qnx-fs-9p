#!/bin/bash
# Launch a mkqnximage-built QNX aarch64le image under qemu-system-aarch64
# (-M virt) with a virtio-9p share on the virtio-mmio transport and v9fs
# tracing, without mkqnximage's runner (which insists on bridge networking).
# No network device is attached.
#
# The VM directory must already contain a built image:
#   cd vm && mkqnximage --type=qemu --arch=aarch64le --graphics=no --noprompt
#
# virtio-mmio slot layout. QEMU virt fills its 32 transports from the top in
# command-line order, and the mkqnximage image hardcodes two of them:
#   slot 31 0x0a003e00  virtio-blk  (devb-virtio smem=0xa003e00,irq=79)
#   slot 30 0x0a003c00  virtio-9p   (mkqnximage puts virtio-net here)
#   slot 29 0x0a003a00  virtio-rng  (devr-virtio mem=0xa003a00)
# so the order of the -device options below matters.
set -euo pipefail

usage() {
    cat <<EOF
usage: $(basename "$0") [options] [-- extra qemu args]

  -v DIR    mkqnximage VM directory (default: \$FS9P_VM_DIR or <repo>/vm)
  -s DIR    host directory to share (default: <vm>/share)
  -t TAG    mount tag (default: hostshare)
  -m MODE   virtio-mmio layout: legacy (version 1, QEMU default) or
            modern (version 2) (default: legacy)
  -S MODEL  9p security_model (default: mapped-xattr)
  -F OPTS   extra -fsdev suboptions, comma-separated
            (e.g. throttling.bps-read=4096 to make requests slow)
  -l FILE   trace log, truncated on start (default: <vm>/qemu-9p.log)
  -H        headless: daemonize, serial on <vm>/serial.sock (logged to
            <vm>/serial.log), monitor on <vm>/monitor.sock
  -k        stop a headless VM started from this VM directory
  -n        print the QEMU command and exit
EOF
}

repo_dir=$(cd "$(dirname "$0")/../.." && pwd)
vm_dir=${FS9P_VM_DIR:-$repo_dir/vm}
share_dir=
tag=hostshare
mode=legacy
sec_model=mapped-xattr
fsdev_extra=
trace_log=
headless=0
stop=0
dry_run=0

while getopts "v:s:t:m:S:F:l:Hknh" opt; do
    case $opt in
    v) vm_dir=$OPTARG ;;
    s) share_dir=$OPTARG ;;
    t) tag=$OPTARG ;;
    m) mode=$OPTARG ;;
    S) sec_model=$OPTARG ;;
    F) fsdev_extra=,$OPTARG ;;
    l) trace_log=$OPTARG ;;
    H) headless=1 ;;
    k) stop=1 ;;
    n) dry_run=1 ;;
    h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

vm_dir=$(realpath "$vm_dir")
pidfile=$vm_dir/qemu.pid

if [ "$stop" = 1 ]; then
    if [ -S "$vm_dir/monitor.sock" ] && command -v socat >/dev/null; then
        echo quit | socat - "UNIX-CONNECT:$vm_dir/monitor.sock" >/dev/null || true
    elif [ -f "$pidfile" ]; then
        kill "$(cat "$pidfile")" 2>/dev/null || true
    fi
    exit 0
fi

opts_file=$vm_dir/output/options
if [ ! -f "$opts_file" ] || [ ! -f "$vm_dir/output/ifs.bin" ]; then
    echo "no built image in $vm_dir (run mkqnximage there first)" >&2
    exit 1
fi
# key='value' shell assignments written by mkqnximage
# shellcheck disable=SC1090
. "$opts_file"
if [ "${OPT_ARCH:-}" != aarch64le ]; then
    echo "only aarch64le images are supported (OPT_ARCH=${OPT_ARCH:-unset})" >&2
    exit 1
fi
if [ "${OPT_BOOT_MODE:-}" = uefi ] || [ "${OPT_IPL:-no}" = yes ]; then
    echo "only direct -kernel boot of output/ifs.bin is supported" >&2
    exit 1
fi

# CPU per mkqnximage's runner (qemu/runimage, OPT_AARCH64_VERSION). Its
# machine types (virt-4.2, virt-7.2) are gone or deprecated in QEMU 10, so
# use virt-8.2 throughout; the virtio-mmio and GIC layout are unchanged.
machine=virt-8.2
case ${OPT_AARCH64_VERSION:-8} in
8) cpu=cortex-a57 ;;
8.2) cpu=cortex-a76 ;;
9) cpu=cortex-a710 ;;
*) echo "unknown OPT_AARCH64_VERSION=$OPT_AARCH64_VERSION" >&2; exit 1 ;;
esac

share_dir=$(realpath -m "${share_dir:-$vm_dir/share}")
trace_log=$(realpath -m "${trace_log:-$vm_dir/qemu-9p.log}")
mkdir -p "$share_dir"

case $mode in
legacy) mmio_args=() ;;
modern) mmio_args=(-global virtio-mmio.force-legacy=false) ;;
*) echo "bad -m mode: $mode" >&2; exit 2 ;;
esac

# x86 hosts have no KVM for aarch64 guests: TCG, one host thread per vCPU.
cmd=(qemu-system-aarch64
    -machine "$machine" -cpu "$cpu" -accel tcg,thread=multi
    -smp "${OPT_CPU:-2}" -m "${OPT_RAM:-1G}"
    -nodefaults -no-user-config
    "${mmio_args[@]}"
    -kernel "$vm_dir/output/ifs.bin"
    -drive "file=$vm_dir/output/disk-qemu.vmdk,if=none,id=drv0"
    -device virtio-blk-device,drive=drv0
    -fsdev "local,id=fsdev0,path=$share_dir,security_model=$sec_model$fsdev_extra"
    -device "virtio-9p-device,fsdev=fsdev0,mount_tag=$tag"
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-device,rng=rng0
    -D "$trace_log" -trace 'v9fs_*'
    -pidfile "$pidfile")

if [ "$headless" = 1 ]; then
    cmd+=(-display none -daemonize
        -chardev "socket,id=ser0,path=$vm_dir/serial.sock,server=on,wait=off,logfile=$vm_dir/serial.log"
        -serial chardev:ser0
        -monitor "unix:$vm_dir/monitor.sock,server=on,wait=off")
else
    cmd+=(-nographic -serial mon:stdio)
fi
cmd+=("$@")

if [ "$dry_run" = 1 ]; then
    printf '%q ' "${cmd[@]}"
    echo
    exit 0
fi

: >"$trace_log"
rm -f "$vm_dir/serial.log"
# QEMU writes its own startup errors to the -D log, not stderr.
if ! "${cmd[@]}"; then
    cat "$trace_log" >&2
    exit 1
fi
