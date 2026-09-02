#!/bin/bash
SP=${SP:?set SP to a dir containing mpcorpus/*.mp and out/}
cd /Users/julien/Documents/GitHub/micropatterns/tools/host_harness
echo -e "script\tcanvas\tpath\trep\tparse\tgen\traster\titems\tskipped"
for f in $SP/mpcorpus/*.mp; do
  n=$(basename $f .mp)
  for dim in "960 540" "200 200"; do
    set -- $dim
    for p in displaylist displaylist-nomap; do
      for rep in 1 2 3 4 5 6 7 8 9; do
        o=$(./build/mpharness render "$f" --width $1 --height $2 --path $p \
              --hour 9 --minute 41 --second 0 --counter 5 --out $SP/out/t.pgm 2>&1)
        echo -e "$n\t$1x$2\t$p\t$rep\t$(echo "$o"|awk '/^  parse/{print $2}')\t$(echo "$o"|awk '/^  displaylist/{print $2}')\t$(echo "$o"|awk '/^  rasterize/{print $2}')\t$(echo "$o"|awk '/display list items/{print $NF}')\t$(echo "$o"|awk '/pixels skipped/{print $NF}')"
      done
    done
  done
done
