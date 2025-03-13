#!/bin/bash
PATH_PWD="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
source ${PATH_PWD}/common.sh


get_config_screen() {
    source /boot/config.txt > /dev/null 2>&1
    echo $screen
}
get_now_screen() {
    source $release_file > /dev/null 2>&1
    echo $lcd
}
now_screen=$(get_now_screen)
config_screen=$(get_config_screen)
# echo "当前  $now_screen "
# echo "配置  $config_screen "
if [ "$now_screen" == "$config_screen" ]; then
    # echo "无事发生"
    exit
fi

# echo "变动  $config_screen "
set-lcd $config_screen install
reboot