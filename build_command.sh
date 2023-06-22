#!/bin/bash
export CROSS_COMPILE=/home/rocky7842/Desktop/Lineage_20/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export ARCH=arm64 && export SUBARCH=arm64

if [  ${1} == "clean" ]
then
    make O=out clean
    make O=out mrproper
fi

make O=out ZS600KL-perf_defconfig
make O=out -j$(nproc --all)

