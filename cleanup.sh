#!/bin/bash

make clean
rm -rf modout
rm .config*
rm arch/arm64/boot/Image.gz-dtb
rm arch/arm64/boot/dtb.img
rm -r arch/arm64/boot/dtb/
