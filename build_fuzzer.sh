#!/bin/bash

#FUZZ_CXXFLAGS="-O2 -fno-omit-frame-pointer -gline-tables-only -fsanitize=address,fuzzer"
#clang++ -g $FUZZ_CXXFLAGS test01.cc -o test01
SRC="fuzz_test/test01.cc"

#-fpermissive: error: assigning to 'char *' from incompatible type 'void *'
#-Wdeprecated: wrning: treating 'c' input as 'c++' when in C++ mode, this behavior is deprecated
#clang++ -g -Wdeprecated -fpermissive -fsanitize=address,fuzzer $SRC -o test01 -I ./include -I ./fuzz_test -I ./fsck -L./lib -lexfat

make clean
CC=clang LDFLAGS="-fsanitize=address,fuzzer-no-link" ./configure
make -j7

echo "archiving objects..."
objcopy --strip-symbol main fsck/fsck.o
ar rcs libfsck.a fsck/fsck.o fsck/repair.o lib/exfat_dir.o lib/exfat_fs.o lib/libexfat.o

echo "building fuzzer..."
clang++ -Wall -fsanitize=fuzzer,address -Wno-cpp -Wpedantic -std=c++11 -g $SRC -o test01 -I ./include -I ./fuzz_test -I ./fsck libfsck.a

cp fuzz_test/test01 ~/qemu-linux/host-share-dir/fuzz_test/
