#!/bin/bash
mkdir -p build
cc -o build/tim tim.c -std=gnu99 -Wall -Wextra "$@"
