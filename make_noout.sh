Path=/home/ravena/OpenSource/a12ltev
project=acer_a12
 
export ARCH=arm
export CROSS_COMPILE=${Path}/prebuilts/gcc/linux-x86/arm/arm-eabi-4.6/bin/arm-eabi-
make mrproper
make ${project}_defconfig
make
