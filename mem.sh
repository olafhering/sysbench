set -x
time for i in mem.*.sh ; do bash $i ; done
grep transferred log.memory.*.txt
