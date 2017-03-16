#!/bin/sh

set -x
autoreconf -vi
env CFLAGS='-fmessage-length=0 -grecord-gcc-switches -fstack-protector -O2 -Wall -D_FORTIFY_SOURCE=2 -funwind-tables -fasynchronous-unwind-tables' \
./configure \
	--without-mysql \
	--without-gcc-arch
