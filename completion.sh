#!/bin/bash
source /boot/config.txt

# 判断是否保存了与overlay_prefix相同的设备树，有才允许该lcd的名称自动补全
link_path=$(readlink -f /usr/bin/set-lcd)
dir_path=$(dirname $link_path)
for dir in $(ls -d "$dir_path/tft/"*/); do
    if [ -d "$dir/dtb" ]; then
        for file in $(ls "$dir/dtb/"*); do
            if [[ $file == *"$overlay_prefix"* ]]; then
                files+="$(basename $dir) "
            fi
        done
    fi
done


_lcd_set () {
    local cur=${COMP_WORDS[COMP_CWORD]}
    if [ $COMP_CWORD -eq 1 ]; then
        COMPREPLY=($(compgen -W "$files" -- $cur))
    elif [ $COMP_CWORD -eq 2 ]; then
        local functions=$(grep -oP '^[^_]\w+\s*\(\)' /usr/bin/set-lcd | sed 's/()//')
        COMPREPLY=($(compgen -W "$functions" -- $cur))
    elif [ $COMP_CWORD -eq 3 ]; then
        local function_name=${COMP_WORDS[2]}
        local variable_name="${function_name}_switch"
        local variable_value=$(grep -oP "(?<=^$variable_name=\").*?(?=\")" /usr/bin/set-lcd)
        COMPREPLY=($(compgen -W "$variable_value" -- $cur))
    fi
}

complete -F _lcd_set set-lcd
