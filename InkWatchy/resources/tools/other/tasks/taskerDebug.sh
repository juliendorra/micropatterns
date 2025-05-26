#!/bin/bash

OPTIONS=("1" "Reset ESP"
         "2" "Reset and monitor"
         "3" "Backtrace resolver"
         "4" "Get coredump"
         "5" "Erase ESP flash"
         "6" "Put device into bootloader mode")

NUM_OPTIONS=$((${#OPTIONS[@]} / 2))

HEIGHT=$((NUM_OPTIONS + 7))
WIDTH=60
CHOICE_HEIGHT=$((NUM_OPTIONS + 1))

TITLE="Debug options"
MENU="Choose:"

CHOICE=$(dialog --clear \
                --backtitle "$TITLE" \
                --title "$TITLE" \
                --menu "$MENU" \
                $HEIGHT $WIDTH $CHOICE_HEIGHT \
                "${OPTIONS[@]}" \
                2>&1 >/dev/tty)

clear
case $CHOICE in
    1)
        resources/tools/other/in/esptool --before default_reset chip_id
        ;;
    2)
        cd resources/tools/other/debug/
        ./resetAndMonitor.sh
        ;;
    3)  
        args=$(dialog --inputbox "Enter the backtrace" 8 150 3>&1 1>&2 2>&3)
        clear
        ./resources/tools/other/debug/backtraceResolver.sh $args
        ;;
    4)
        cd resources/tools/other/debug/
        ./getCoreDump.sh
        ;;
    5)
        resources/tools/other/in/esptool erase_flash
        ;;
    6)
        resources/tools/other/in/esptool --after no_reset chip_id
        ;;
esac
