#!/bin/bash
source resources/tools/globalFunctions.sh

envList=("Watchy_1" "Watchy_1_5" "Watchy_2" "Watchy_3" "Yatchy")

# Also don't forget removing generating the binary when removing things from this list
#envList=("Watchy_2")

for env in "${envList[@]}"; do
    pio run --target clean -e $env
    pio pkg install -e $env
    pio pkg update -e $env
done

function generalThings {
    mkdir trash/
    mv resources/personal/books trash/
    cp resources/personal/moduleImages trash/
    cp -r resources/demo/* resources/personal/

    rm -rf src/defines/confidential.h
    rm -rf src/defines/config.h

    # To generate config files
    cd resources/tools/
    ./generate.sh
    cd ../../

    old_string="#define VAULT_PASSWORD \"\""
    new_string="#define VAULT_PASSWORD \"5\""
    filename="src/defines/confidential.h"

    sed -i "s/$old_string/$new_string/g" "$filename"

    old_string="#define VAULT 0"
    new_string="#define VAULT 1"
    filename="src/defines/config.h"

    sed -i "s/$old_string/$new_string/g" "$filename"

    old_string="#define BOOK 0"
    new_string="#define BOOK 1"
    filename="src/defines/config.h"

    sed -i "s/$old_string/$new_string/g" "$filename"

    # old_string="#define WIFI_SSID1 \"\""
    # new_string="#define WIFI_SSID1 \"hotspot\""
    # filename="src/defines/confidential.h"

    # sed -i "s/$old_string/$new_string/g" "$filename"

    # old_string="#define WIFI_PASS1 \"\""
    # new_string="#define WIFI_PASS1 \"12345678\""
    # filename="src/defines/confidential.h"

    # sed -i "s/$old_string/$new_string/g" "$filename"

    old_string="#define WEATHER_LATIT \"\""
    new_string="#define WEATHER_LATIT \"53.55073\""
    filename="src/defines/confidential.h"

    sed -i "s/$old_string/$new_string/g" "$filename"

    old_string="#define WEATHER_LONGTIT \"\""
    new_string="#define WEATHER_LONGTIT \"9.99302\""
    filename="src/defines/confidential.h"

    sed -i "s/$old_string/$new_string/g" "$filename"

    # To regenerate vault
    cd resources/tools/
    ./generate.sh
    cd ../../

    cd resources/tools/fs
    ./createFs.sh
    cd ../../../
}

generalThings

function compileEnv {
    pio run -e $1
}

for env in "${envList[@]}"; do
    compileEnv $env
done

function createEmptyBinary {
    dd if=/dev/zero of=demo.bin bs=1M count=$1
}

function hexToDec {
  printf "%d\n" "$1"
}

function writeAtOffset {
    # Input into output
    local outputFile=$1
    local inputFile=$2
    local offset=$3
    dd if="$inputFile" of="$outputFile" bs=1 seek="$offset" conv=notrunc
}

function assembleBinary {
    binSize=$1
    envName=$2
    bootloaderAddress=$(hexToDec $3)

    bootloaderPath=".pio/build/$envName/bootloader.bin"
    partitionTablePath=".pio/build/$envName/partitions.bin"
    firmwarePath=".pio/build/$envName/firmware.bin"
    resourcesPath="resources/tools/fs/out/fs.bin"

    createEmptyBinary $binSize

    bootloaderAddress=$(hexToDec $3)
    partitionTableAddress=$(hexToDec 0x19000)
    firmwareAddress=$(hexToDec 0x20000)
    resourceAddress=$(<resources/tools/fs/in/offset.txt tr -d '\n')

    writeAtOffset demo.bin $bootloaderPath $bootloaderAddress
    writeAtOffset demo.bin $partitionTablePath $partitionTableAddress
    writeAtOffset demo.bin $firmwarePath $firmwareAddress
    writeAtOffset demo.bin $resourcesPath $resourceAddress

    mv demo.bin resources/tools/other/out/demo/${envName}-demo.bin
}

rm -rf resources/tools/other/out/demo/
mkdir -p resources/tools/other/out/demo/

assembleBinary 4 Watchy_1 0x1000
assembleBinary 4 Watchy_1_5 0x1000
assembleBinary 4 Watchy_2 0x1000
assembleBinary 8 Watchy_3 0x0
assembleBinary 4 Yatchy 0x0

touch resources/personal/books/.gitkeep
touch resources/personal/vault/.gitkeep

sync