set -e
out=$1
test -n "${out}"
t=`mktemp --tmpdir=/dev/shm`
trap "rm -f ${t}" EXIT
while :
do
  echo "`xenstore-read domid 2>&1`:`xenstore-read image/device-model-pid 2>&1`" &> ${t}
  mv -f "${t}" "${out}"
  sleep 2
done
