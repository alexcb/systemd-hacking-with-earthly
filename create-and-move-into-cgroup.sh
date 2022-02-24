#!/bin/sh
set -ex

test "$$" = "1"

ls -la /sys/fs/cgroup
mkdir /sys/fs/cgroup/foo.scope
echo 1 > /sys/fs/cgroup/foo.scope/cgroup.procs

echo "running, sleeping for 10"
sleep 10
echo "exiting"
