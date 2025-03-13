#!/bin/bash
echo "开始编译"

if ! ls -d /usr/lib/modules/*/ > /dev/null 2>&1; then
    echo "不存在非隐藏的子文件夹，退出脚本。"
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
