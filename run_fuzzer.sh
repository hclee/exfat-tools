#!/bin/bash

export LLVM_PROFILE_FILE="fuzz_coverage.profraw"

# len_control: 0 means no length control
./test_fsck fuzz_test/corpus -runs=1000000 \
	-max_len=5242880 -len_control=0 -prefer_small=0 \
	-print_pcs=1 -print_final_stats=1 -ignore_crashes=1 -dump_coverage=1 \
	| tee -a test_fsck.log

# with this option, the fuzzer cannot detect all inputs of corpus
#-fork=1 
