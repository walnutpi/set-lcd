#!/bin/bash
echo "开始编译"

if ! ls -d /usr/lib/modules/*/ > /dev/null 2>&1; then
    echo "不存在非隐藏的子文件夹，退出脚本。"
    return
fi

PATH_PWD="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
PATH_DRIVER="${PATH_PWD}/drivers"
MODULE_PATH="$(ls -d /lib/modules/* | head -n 1)"

PATH_DRIVER_SAVE="${MODULE_PATH}/kernel/wpi-drivers/"


echo "compile drivers"
make -C ${PATH_DRIVER}

if [ -d $PATH_DRIVER_SAVE ]; then
    rm -r $PATH_DRIVER_SAVE
fi
mkdir -p $PATH_DRIVER_SAVE
cp -r ${PATH_DRIVER}/* $PATH_DRIVER_SAVE

echo -e "\nupdate system drivers...."

kernel_versions=($(ls -1 "/lib/modules" 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.'))
depmod -a "$kernel_versions"