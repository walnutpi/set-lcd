#!/bin/bash
echo "开始编译"


KERNEL="/usr/lib/modules/$(uname -r)/build/"
echo "KERNEL: $KERNEL"
if [ ! -d $KERNEL ]; then
    echo " $KERNEL not exist"
    return
fi


echo "compile drivers"
make -C ${PATH_DRIVER}
if [ -d $PATH_DRIVER_SAVE ]; then
    rm -r $PATH_DRIVER_SAVE
fi
mkdir -p $PATH_DRIVER_SAVE
cp -r ${PATH_DRIVER}/* $PATH_DRIVER_SAVE

echo -e "\nupdate system drivers...."
depmod
