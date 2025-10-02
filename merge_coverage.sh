#!/bin/bash
#
llvm-profdata merge -sparse fuzz_coverage.profraw -o fuzz_coverage.profdata
