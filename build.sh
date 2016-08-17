#!/bin/sh

LD_FLAGS="-flto"
CFLAGS="-Wall -Ofast -march=native -flto"

gcc ${CFLAGS} -fpic -finput-charset=cp932 -I./ -D IN_SHARED_MODULE -c mod_tcpcast.c
gcc -Wall -shared -fpic -o mod_tcpcast.so mod_tcpcast.o ${LD_FLAGS}
rm -f mod_tcpcast.o
