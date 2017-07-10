set -x
tag=$1
if test -n "${tag}"
then
  for cnt in {1..5}
  do
    time for i in mem.1?.*.sh ; do bash $i ; done
    grep transferred log.memory.*.txt
    mkdir -vp "${tag}.${cnt}"
    mv -vit "$_" log.memory.*.txt
  done
fi
