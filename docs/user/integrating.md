# Adding fs-9p to a QNX system

The guest needs the `fs-9p` binary in its image and a command that starts
it at boot. fs-9p needs only `procnto` and two shared libraries, `libc.so.6` and
`libgcc_s.so.1`, which are in the IFS of mkqnximage images (put them in
yours if you write the buildfile yourself), so it can start early. The mount point doesn't have to exist: fs-9p attaches it in the
path namespace.

`share/fs-9p/fs-9p.build` under the installation prefix (the source copy is
`packaging/fs-9p.build`) has the lines below ready to copy.

## Images made with mkqnximage

Two snippet files in the image directory, then a rebuild:

`local/snippets/system_files.custom` puts the binary in `/system/bin`:

```
[perms=0755] bin/fs-9p=/opt/fs-9p/sbin/fs-9p
```

`local/snippets/post_start.custom` mounts the share at the end of startup:

```
fs-9p hostshare /mnt/host
```

```
mkqnximage --build --noprompt
```

Use the absolute host path of the installed binary. After the next boot
`/mnt/host` is there; `pidin -p fs-9p` shows the process.

## Your own IFS buildfile

Put fs-9p in the IFS (in `/proc/boot`) and start it from the startup
script, before anything that uses the share:

```
[+script] .script = {
    ...
    fs-9p hostshare /mnt/host
    waitfor /mnt/host 10
    ...
}

[perms=0755] fs-9p=/opt/fs-9p/sbin/fs-9p
```

`fs-9p` returns once the share is mounted and keeps running in the
background (it daemonizes); `waitfor` is there for scripts that continue
regardless. With mkqnximage, the same lines go into
`local/snippets/ifs_files.custom` (the file line) and
`local/snippets/ifs_start.custom` (the two commands).

## Running without root

fs-9p needs three privileges, which root has. Any other user needs exactly
these abilities:

| Ability | For |
|---|---|
| `mem_phys` | Mapping the virtio device's registers |
| `interrupt` | Attaching to the device's interrupt |
| `pathspace` | Attaching the mount point in the path namespace |

With `on`:

```
on -u qnxuser -A nonroot,allow,mem_phys -A nonroot,allow,interrupt \
    -A nonroot,allow,pathspace fs-9p hostshare /mnt/host
```

Without one of them fs-9p stops at startup with "Operation not permitted".
Systems that use security policies (`secpol`) grant the same three
abilities to fs-9p's type instead; that setup hasn't been tested.

The user fs-9p runs as doesn't affect who can use the files: fs-9p checks
each client's permissions itself ({doc}`behaviour`).

## Several shares

One fs-9p per mount tag, each at its own mount point:

```
fs-9p src /mnt/src
fs-9p -o ro tools /opt/tools
```

`ls /dev/name/local/fs-9p` lists the tags being served.

## Options for a production system

The defaults suit most uses. Consider:

- `ro` when the guest only reads the share.
- `cache=MS` for trees that change rarely on the host (toolchains, large
  source trees): `stat` and lookups get much faster, host changes show up
  to MS milliseconds late. Off by default.
- `timeout=MS` (default 30000): how long a request may take before the
  call fails with "Connection timed out". Lower it to notice a stuck host
  sooner; 0 waits forever.
- `requests=N` (default 4) and `msize=N` (default 512 KiB): memory against
  parallel throughput, see {doc}`performance`.

All options: {doc}`reference`.

## Shutdown and restart

`slay fs-9p` unmounts cleanly: fs-9p removes the mount point, waits for
requests in flight, resets the device and exits. Do it before shutting the
guest down if anything may still be writing. `slay` returns at once, while
fs-9p may still be finishing requests; a new fs-9p for the same tag can
start once the old one has exited (`/dev/name/local/fs-9p/TAG` is gone),
and also after a crash or `slay -s KILL` (it resets the device the old one
left behind).
