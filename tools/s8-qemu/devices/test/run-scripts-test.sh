#!/bin/bash
export PATH=$PATH:/root/work/s8-qemu/qemu-e-scratch/build
bash "$(dirname "$0")/scripts-test.sh"
