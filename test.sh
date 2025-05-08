#!/bin/bash

echo "Running tests..."
make clean
make riscv >/dev/null
make run >os.out