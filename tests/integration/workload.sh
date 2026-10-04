#!/bin/sh
# A build-like file workload, for cross-development checks. write.sh runs
# it once on the host and once in the guest on the share; the two result
# trees must be identical. It touches what a build does: unpack, patch,
# edit in place, write to a temporary name and rename over the target,
# replace existing outputs, clean up intermediates, rename the output
# directory. No symlinks: with security_model=mapped-xattr a symlink made in
# the guest is a regular file on the host, which a host-side diff would
# report (symlinks have their own checks in write.sh).
#
# usage: workload.sh INPUT_DIR WORK_DIR
#   INPUT_DIR holds src.tar (ustar) and change.patch (-p1, against src/).
# Portable sh: runs under bash with GNU tools and under QNX ksh with toybox.
set -e
LC_ALL=C
export LC_ALL
in=$1
work=$2

mkdir -p "$work"
cd "$work"
tar xf "$in/src.tar"
(cd src && patch -p1 -s <"$in/change.patch")
sed -i 's/@VERSION@/1.2.3/' src/version.h

# "Compile" each source twice: the second pass replaces existing outputs
# through a temporary file, as compilers and make recipes do.
mkdir obj
for pass in 1 2; do
    for c in src/*.c; do
        o=obj/$(basename "$c" .c).o
        { echo "pass $pass"; cksum <"$c"; cat "$c"; } >"$o.tmp"
        mv "$o.tmp" "$o"
    done
done

mkdir -p out/logs
cat obj/*.o >out/app.bin
cp src/version.h out/
echo "built $(ls obj | wc -l | tr -d ' ') objects" >out/logs/build.log
rm -r obj
mv out dist
