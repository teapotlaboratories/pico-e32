#!/usr/bin/env bash
# Host compile + logic tests of the input component: the fc-scheduled backend (components/input/input_scheduled.c) against
# stub ESP-IDF headers — validates the C compiles AND that its ring/parser/apply-by-fc logic matches the
# Python twin (test/playtest/fc_sched.py). No ESP-IDF / hardware needed. The device build/flash is separate.
#   components/input/host_test/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
gcc -std=gnu11 -Wall -Wextra -Wno-unused-variable \
    -I "$HERE/stubs" -I "$HERE/.." \
    "$HERE/test_input_scheduled.c" -o "$HERE/tis"

gcc -std=gnu11 -Wall -Wextra -Wno-unused-variable \
    -I "$HERE/stubs" -I "$HERE/.." \
    "$HERE/test_input_exit.c" -o "$HERE/tie"
"$HERE/tie"
"$HERE/tis"

gcc -std=gnu11 -Wall -Wextra -Wno-unused-variable \
    -I "$HERE/stubs" -I "$HERE/.." \
    "$HERE/test_input_exit.c" -o "$HERE/tie"
"$HERE/tie"
