# Quickstart

From nothing to a host directory mounted in a QNX guest. You need a Linux
host with the QNX SDP 8.0 (with the QEMU `virt` packages, see
{doc}`overview`), CMake 3.20 or later and QEMU 8.2 or later
(`qemu-system-aarch64`).

## 1. Build fs-9p

```
source ~/qnx800/qnxsdp-env.sh
git clone <fs-9p repository> fs-9p && cd fs-9p
cmake -S . -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake
cmake --build build-qnx
cmake --install build-qnx --prefix ~/fs-9p-install
```

The binary is now `~/fs-9p-install/sbin/fs-9p`.

## 2. Make a QNX image that contains it

`mkqnximage` comes with the SDP. In a new directory:

```
mkdir ~/qnxvm && cd ~/qnxvm
mkqnximage --type=qemu --arch=aarch64le --graphics=no --noprompt
echo "[perms=0755] bin/fs-9p=$HOME/fs-9p-install/sbin/fs-9p" >> local/snippets/system_files.custom
mkqnximage --build --noprompt
```

The first command creates the image and its `local/snippets/` files; the
line added to `system_files.custom` puts fs-9p in `/system/bin`; the second
`mkqnximage` rebuilds the image with it.

## 3. Start QEMU with a shared directory

```
mkdir -p ~/shared && echo "Hello from the host" > ~/shared/hello.txt
cd ~/qnxvm
qemu-system-aarch64 -M virt-8.2 -cpu cortex-a57 -smp 2 -m 1G \
    -accel tcg,thread=multi -nographic -nodefaults -no-user-config \
    -serial mon:stdio -kernel output/ifs.bin \
    -drive file=output/disk-qemu.vmdk,if=none,id=drv0 \
    -device virtio-blk-device,drive=drv0 \
    -fsdev local,id=fs0,path=$HOME/shared,security_model=mapped-xattr \
    -device virtio-9p-device,fsdev=fs0,mount_tag=hostshare \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-device,rng=rng0
```

Keep the devices in this order: the image's own drivers expect the disk and
the random number generator at fixed places (see {doc}`qemu`). Booting
takes about 20 seconds under emulation; wait for `Startup complete`.
`Ctrl-a x` quits QEMU.

## 4. Mount the share in the guest

At the guest's shell:

```
fs-9p hostshare /mnt/host
cat /mnt/host/hello.txt
echo "Hello from QNX" > /mnt/host/reply.txt
```

`hello.txt` shows the host's text, and `~/shared/reply.txt` appears on the
host. `slay fs-9p` unmounts.

## Next

- Start fs-9p at boot, run it without root, or add it to your own image:
  {doc}`integrating`.
- Choose the QEMU options for your use: {doc}`qemu`.
- Every option of `fs-9p`: {doc}`reference`.
