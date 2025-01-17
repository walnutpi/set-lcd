#!/bin/bash

_lcd_set () {
    local cur=${COMP_WORDS[COMP_CWORD]}
    if [ $COMP_CWORD -eq 1 ]; then
        local link_path=$(readlink -f /usr/bin/set-lcd)
        local dir_path=$(dirname $link_path)
        local files=$(ls -d "$dir_path/tft/"*/ |  sed "s#$dir_path/tft/##"| sed 's#/##')
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
