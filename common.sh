#!/bin/bash

release_file="/etc/WalnutPi-release"

_file_insert() {
    if ! grep -q "^$2" "$1"; then
        echo "$2" >> "$1"
    fi
}
_file_delete() {
    if grep -q "^$2" "$1"; then
        sed -i "/$2/d" $1
    fi
}

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


_add_flag() {
    _try_command echo "lcd=enable" >> $release_file
}

_delete_flag() {
    sed -i '/lcd=enable/d' $release_file
}
