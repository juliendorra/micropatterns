#!/bin/bash
SP=${SP:?set SP to a dir containing mpcorpus/*.mp and out/}
cd /Users/julien/Documents/GitHub/micropatterns/tools/host_harness
echo -e "script\tw\th\tseed\titems\trendered\toffscreen\toccluded\tskipped\tnonwhite\tgen_ms\traster_ms"
for f in $SP/mpcorpus/*.mp; do
  n=$(basename $f .mp)
  for dim in "960 540" "200 200"; do
    set -- $dim
    for hh in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23; do
      mm=$(( (hh*7) % 60 )); ss=$(( (hh*13) % 60 )); cc=$hh
      o=$(./build/mpharness render "$f" --width $1 --height $2 \
            --counter $cc --hour $hh --minute $mm --second $ss \
            --out $SP/out/tmp.pgm 2>&1)
      it=$(echo "$o" | awk '/display list items/{print $NF}')
      rd=$(echo "$o" | awk '/rendered items/{print $NF}')
      os=$(echo "$o" | awk '/culled off-screen/{print $NF}')
      oc=$(echo "$o" | awk '/culled by occlusion/{print $NF}')
      sk=$(echo "$o" | awk '/pixels skipped/{print $NF}')
      nw=$(echo "$o" | awk '/non-white pixels/{print $3}')
      g=$(echo "$o"  | awk '/^  displaylist/{print $2}')
      r=$(echo "$o"  | awk '/^  rasterize/{print $2}')
      echo -e "$n\t$1\t$2\th$hh\t$it\t$rd\t$os\t$oc\t$sk\t$nw\t$g\t$r"
    done
  done
done
