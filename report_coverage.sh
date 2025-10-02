#!/bin/bash

# generate html
#llvm-cov show ./test_fsck -instr-profile=fuzz_coverage.profdata -object=libfsck.a -format=html > index.html
llvm-cov report ./test_fsck -instr-profile=fuzz_coverage.profdata -object=libfsck.a
