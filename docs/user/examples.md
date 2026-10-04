# Examples

## Build on the host, run in the guest

Share the build tree, build with `qcc` on the host, run the result in the
guest without copying anything.

Host (QEMU options and build):

```
-fsdev local,id=fs0,path=/home/me/project,security_model=mapped-xattr
-device virtio-9p-device,fsdev=fs0,mount_tag=project
```

```
qcc -Vgcc_ntoaarch64le -o build/mytest mytest.c
```

Guest:

```
fs-9p project /mnt/project
/mnt/project/build/mytest > /mnt/project/build/mytest.log
```

The log is on the host as soon as `mytest` writes it. Programs on the share
run with the caller's identity: setuid and setgid bits are ignored.

## A read-only toolchain with a metadata cache

Tools and libraries that the guest only reads, on a tree the host doesn't
change while the guest runs:

```
-fsdev local,id=fs1,path=/opt/qnx-tools,security_model=none,readonly=on
-device virtio-9p-device,fsdev=fs1,mount_tag=tools
```

```
fs-9p -o ro,cache=10000 tools /opt/tools
```

Lookups and `stat`s are answered from the cache for up to 10 seconds.

## Two shares

```
-fsdev local,id=fs0,path=/home/me/src,security_model=mapped-xattr
-device virtio-9p-device,fsdev=fs0,mount_tag=src
-fsdev local,id=fs1,path=/home/me/results,security_model=mapped-xattr
-device virtio-9p-device,fsdev=fs1,mount_tag=results
```

```
fs-9p src /mnt/src
fs-9p results /mnt/results
```

With an mkqnximage image, these devices go after the disk, the first one
before the random-number device ({doc}`qemu`).

## Mount at boot, as a non-root user

In an mkqnximage image's `local/snippets/post_start.custom`:

```
on -u qnxuser -A nonroot,allow,mem_phys -A nonroot,allow,interrupt -A nonroot,allow,pathspace fs-9p hostshare /mnt/host
```

## Copy a tree in and out

```
cp -r /mnt/host/input /data/input
cp -r /data/output /mnt/host/output
```

Both directions keep file contents exactly (checked with `diff -r` in the
fs-9p tests). Ownership of the copies follows the QEMU `security_model`
({doc}`qemu`).

## Look at what fs-9p does

Run it in the foreground with every 9P message logged, and have QEMU log
the requests it receives:

```
-trace 'v9fs_*' -D /tmp/qemu-9p.log            (QEMU option, host)
```

```
slay fs-9p
fs-9p -o debug hostshare /mnt/host 2> /tmp/fs-9p.log &
ls -l /mnt/host
```

See {doc}`troubleshooting` for reading these logs.
