#!/bin/bash

release_file="/etc/WalnutPi-release"

FLAG_HDMI="hdmi"


# 判断config.txt里有没有以参数1开头的行
# 如果有则把后续部分替换为参数2
# 如果没有则插入一行以参数1和2拼接的字符串，
_config_insert() {
    local key="$1"
    local value="$2"
    local file="/boot/config.txt"
    if grep -q "^$key" "$file"; then
        sudo sed -i "/^$key/c\\$key$value" "$file"
    else
        echo "$key$value" | sudo tee -a "$file"
    fi
}

_try_command() {
    set +e
    "$@"
    set -e
}


_flag_modify(){
    flag=$1
    flag_prefix="lcd="
    flag_str="${flag_prefix}${flag}"
    if grep -q "^$flag_prefix" "$release_file"; then
        sed -i "/$flag_prefix/d" "$release_file"
    fi
    echo "$flag_str" >> "$release_file"
}


# 插入驱动名称到modules文件
_enable_modules() {
    module=$1
    modules_file="/etc/modules"
    if ! grep -q "$module" "$modules_file"; then
        echo "$module" >> "$modules_file"
    fi
}
# 从modules文件中删除驱动名称
_disable_modules() {
    module=$1
    modules_file="/etc/modules"
    if grep -q "$module" "$modules_file"; then
        sed -i "/$module/d" "$modules_file"
    fi
}

# 传入存放设备树的文件夹路径，将所有文件编译后存放到overlays文件夹
_updoad_dtbo() {
    dtb_file_path=$1
    dts_files=$(find "$dtb_file_path" -type f -name "*.dts")
    for dts_file in $dts_files; do
        dtbo_file="${dts_file%.dts}.dtbo"
        dtc -@  -I dts -O dtb -o "$dtbo_file" "$dts_file"
    done
    
    overlays_folder="/boot/overlays"
    dtb_files=$(find "$dtb_file_path" -type f -name "*.dtbo")
    for file in $dtb_files; do
        cp "$file" "$overlays_folder"
    done
    
}
# 修改设备树中指定属性的数值
_dtb_replace_value() {
    local file_path=$1
    local name=$2
    local value=$3

    local searchString="$name = <[0-9]*>"
    local replaceString="$name = <$value>"
    
    sed -i "s/$searchString/$replaceString/g" $file_path
}

