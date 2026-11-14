#!/bin/sh
# Regression: recompiled zlib + Lua must produce the recorded output.
cd "$(dirname "$0")/.." || exit 1
./build/test_lua orig.bin tests/basic.lua 2>&1 | diff -u tests/basic.expected - && echo "PASS basic.lua"
