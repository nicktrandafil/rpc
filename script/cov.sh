#!/bin/bash

if [ ! -d dcov ]; then mkdir dcov; fi
./test_rpc
lcov --ignore-errors mismatch --directory . --capture --output-file dcov/coverage.info              # capture coverage info
lcov --ignore-errors mismatch --remove dcov/coverage.info '/usr/*' --output-file dcov/coverage.info # filter out system
genhtml dcov/coverage.info -o dcov
lcov --list dcov/coverage.info
