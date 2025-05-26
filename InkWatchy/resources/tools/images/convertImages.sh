#!/bin/bash
source ../globalFunctions.sh

generate_img () {
    local f=$1
    local output_path=$2 # Without / at the end

    img_name="${f##*/}"
    img_name="${img_name%.*}"
    img_path="$output_path/${img_name}"
    #img_info="out/${img_name}Inf"

    identify -ping -format '%w\n%h' $f > out/info #${img_info}

    ../other/in/magick $f -dither FloydSteinberg -define dither:diffusion-amount=90% -remap img/eink-2color.png -depth 1 gray:- > out/data #${img_path}
    
    g++ -I out/ imgDumper.cpp -o out/imgDumper
    out/imgDumper

    rm -rf out/data
    rm -rf out/info
    rm -rf out/imgDumper
    mv out/img.bin $img_path
}

rm -rf out 1>/dev/null 2>/dev/null
mkdir -p out

# Generate condition images
for dir in img/condition/*/; do    
    if [ -d "$dir" ]; then
        # echo "Checking directory: ${dir}"

        if [ -f "$dir/define.txt" ]; then
            define=$(<"$dir/define.txt")
            # echo "The define is: ${define}"

            check_define "$define" "../../../src/defines/config.h"
            exists=$?
            # echo "Is define enabled: ${exists}"
            if [ $exists -eq 1 ]; then
                last_dir=$(basename "$dir")
                echo "The define ${define} in ${dir} is turned on"
                mkdir -p out/$last_dir
                for f in $dir/*
                do
                    if [[ $f == *".txt"* ]]; then
                        continue
                    fi
                    echo "Processing $f"
                    generate_img $f out/$last_dir
                done
            else
                echo "The define ${define} in ${dir} is turned off"
            fi
        fi
    fi
done

# Generate regular images
for f in img/*
do
    if [[ $f == *".sh"* ]] || [[ $f == *".h"* ]] || [[ $f == "eink-2color.png" ]] || [[ $f == *".txt"* ]]; then
        continue
    fi

    if [ ! -f "$f" ]; then
        continue
    fi

    echo "Processing $f"

    generate_img $f "out"
done

# Generate module images
mkdir out/watchfaceImages
for f in ../../personal/moduleImages/*
do
    if [[ $f == *".gitkeep"* ]]; then
        continue
    fi

    if [ ! -f "$f" ]; then
        continue
    fi

    echo "Processing $f"

    generate_img $f "out/watchfaceImages"
done

rm -rf ../fs/littlefs/img/ 1>/dev/null 2>/dev/null
mv out ../fs/littlefs/img/