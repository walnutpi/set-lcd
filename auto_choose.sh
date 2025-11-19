#!/bin/bash
PATH_PWD="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"
source ${PATH_PWD}/common.sh
FLAG_FILE="/etc/screen-later"

if [ -f $FLAG_FILE ]; then
    source $FLAG_FILE
    set-lcd $screen install
    rm $FLAG_FILE
    exit
fi

get_config_screen() {
    source /boot/config.txt >/dev/null 2>&1
    echo $screen
}
get_now_screen() {
    source $release_file >/dev/null 2>&1
    echo $lcd
}
now_screen=$(get_now_screen)
config_screen=$(get_config_screen)
# echo "当前  $now_screen "
# echo "配置  $config_screen "

# t527开启gpu的配置和lcd的fbdev冲突，只能留一个
auto_backup_gpu_config_file() {
    gpu_config_file="/etc/X11/xorg.conf.d/20-modesetting.conf"
    fbdev_config_file="/usr/share/X11/xorg.conf.d/99-fbdev.conf"
    if [[ "$now_screen" == lcd* ]]; then
        if [ ! -e /dev/fb1 ]; then
            # 备份gpu配置，使能fbdev配置
            if [ -f "$gpu_config_file" ]; then
                mv "$gpu_config_file" "${gpu_config_file}.bak"
            fi
            if [ -f "${fbdev_config_file}.bak" ]; then
                mv "${fbdev_config_file}.bak" "$fbdev_config_file"
            fi
        else
            # 备份fbdev配置，使能gpu配置
            if [ -f "$fbdev_config_file" ]; then
                mv "$fbdev_config_file" "${fbdev_config_file}.bak"
            fi
            if [ -f "${gpu_config_file}.bak" ]; then
                mv "${gpu_config_file}.bak" "$gpu_config_file"
            fi
        fi
    else
        # 恢复gpu配置文件（如果存在备份）
        if [ -f "${gpu_config_file}.bak" ]; then
            mv "${gpu_config_file}.bak" "$gpu_config_file"
        fi
    fi
}
# t527上如果设置为dsi屏幕，但dsi屏幕通讯失败了，会导致后续其他驱动报错死机
auto_close_dsi_on_dsi_fail() {
    if [[ "$now_screen" == dsi* ]]; then
        if dmesg | grep -q "panel-dsi driver not probe"; then
            echo "screen=$config_screen" >$FLAG_FILE
            config_screen="hdmi"
        fi
    fi
}
# 如果屏幕配置有变化，则重启
reboot_if_screen_changed() {
    if [ "x$now_screen" != "x$config_screen" ]; then
        set-lcd $config_screen install
        reboot
    fi
}
auto_backup_gpu_config_file
auto_close_dsi_on_dsi_fail
reboot_if_screen_changed
