#!/bin/bash
set -x
sysbench=$1
test -z "${sysbench}" && sysbench=./src/sysbench
LOOP=$2
test -z "${LOOP}" && LOOP=1
cpus=`if grep -Ec 'cpu[0-9]' /proc/stat ; then : ; elif sysctl -n hw.ncpu ; then : ; else echo 1 ; fi`

while test $LOOP -gt 0; do
  echo Test $LOOP
  uname -a
  free -m
  ${sysbench} \
--mutex-num=$(( 4096 * 1 )) \
--mutex-locks=$(( 0xffff )) \
--mutex-loops=$(( 0xffff )) \
        --verbosity=5 \
        --threads=${cpus} \
        --report-interval=3 \
        --time=65536 \
        mutex run
        date
        uname -a
        sleep 2
  LOOP=$(($LOOP-1))
done 2>&1 | tee log.txt
