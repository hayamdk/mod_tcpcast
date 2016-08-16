#!/bin/sh

LD_FLAGS=

gcc -Wall -Ofast -march=native -fpic -flto -finput-charset=cp932 -I./ -D IN_SHARED_MODULE -c mod_tcpcast.c
gcc -Wall -shared -fpic -flto -o mod_tcpcast.so mod_tcpcast.o ${LD_FLAGS}
rm -f mod_tcpcast.o
