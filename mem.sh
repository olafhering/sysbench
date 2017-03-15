#!/bin/bash
set -x
sysbench=$1
LOOP=4
cpus=`if grep -Ec 'cpu[0-9]' /proc/stat ; then : ; elif sysctl -n hw.ncpu ; then : ; else echo 1 ; fi`

while test $LOOP -gt 0; do
  echo Test $LOOP
  ${sysbench} \
        --memory-block-size=8k \
        --memory-total-size=1024T \
        --verbosity=5 \
        --threads=${cpus} \
        --report-interval=3 \
        --time=1234 \
        memory run
        date
        uname -a
        sleep 2
  LOOP=$(($LOOP-1))
done 2>&1 | tee log.txt
