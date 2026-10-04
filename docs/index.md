# fs-9p

fs-9p mounts a host directory in a QNX 8.0 guest running under QEMU. It is
a QNX resource manager that speaks 9P2000.L to QEMU's virtio-9p device on
the aarch64 `virt` machine, over the virtio-mmio transport: no network, no
extra daemon on the host. QEMU itself is the file server.

```
host:   qemu-system-aarch64 -M virt ... \
          -fsdev local,id=fs0,path=/some/dir,security_model=mapped-xattr \
          -device virtio-9p-device,fsdev=fs0,mount_tag=hostshare
guest:  fs-9p hostshare /mnt/host
```

This documentation has two parts:

- The **User Guide** is for system integrators: building fs-9p, setting up
  QEMU, adding fs-9p to a QNX image, the `fs-9p` reference, behaviour and
  limitations, examples and troubleshooting.
- The **Developer Guide** is for whoever maintains fs-9p: the detailed
  design, the test environment and the tests, the development workflow,
  making a release and updating these docs.

```{toctree}
:maxdepth: 2

user/index
dev/index
changelog
```
