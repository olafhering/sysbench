#!/bin/sh

set -xe
autoconf --version
libtool --version
automake --version
pkg-config --version
make --version
gcc --version
test -n "$(type -p gcc-4.7)" && gcc-4.7 --version
test -n "$(type -p gcc-4.8)" && gcc-4.8 --version
xxd --version
CC=gcc
test -n "$(type -p gcc-4.7)" && CC=gcc-4.7
test -n "$(type -p gcc-4.8)" && CC=gcc-4.8
autoreconf -vi
env \
CC=$CC \
CFLAGS='-fmessage-length=0 -fstack-protector -O2 -Wall -D_FORTIFY_SOURCE=2 -funwind-tables -fasynchronous-unwind-tables' \
./configure \
	--without-mysql \
	--without-gcc-arch
