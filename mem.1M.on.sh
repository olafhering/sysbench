#!/bin/bash
set -x
sysbench=$1
test -z "${sysbench}" && sysbench=./src/sysbench
LOOP=$2
test -z "${LOOP}" && LOOP=1
cpus=`if grep -Ec 'cpu[0-9]' /proc/stat ; then : ; elif sysctl -n hw.ncpu ; then : ; else echo 1 ; fi`
blk=1M
exectimes=on

tracker=/dev/shm/$$
bash domid_tracker.sh ${tracker} < /dev/null &> /dev/null &
tracker_pid=$!
trap "kill -SIGHUP ${tracker_pid}" EXIT

while test $LOOP -gt 0; do
  echo Test $LOOP
  uname -a
  free -m
  ${sysbench} \
        --memory-block-size=${blk} \
        --memory-total-size=8388607T \
        --verbosity=5 \
        --threads=${cpus} \
        --report-interval=3 \
	--memory-exectimes=${exectimes} \
	--memory-tracker=${tracker} \
        --time=66 \
        memory run
        date
        uname -a
        sleep 2
  LOOP=$(($LOOP-1))
done 2>&1 | tee log.memory.${blk}.${cpus}.${exectimes}.txt
