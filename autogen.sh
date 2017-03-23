#!/bin/sh

set -xe
autoconf --version
libtool --version
automake --version
pkg-config --version
make --version
gcc --version
xxd --version
autoreconf -vi
env CFLAGS='-fmessage-length=0 -fstack-protector -O2 -Wall -D_FORTIFY_SOURCE=2 -funwind-tables -fasynchronous-unwind-tables' \
./configure \
	--without-mysql \
	--without-gcc-arch
