#!/bin/bash
SP=${SP:?set SP to a dir containing mpcorpus/*.mp and out/}
cd /Users/julien/Documents/GitHub/micropatterns/tools/host_harness
echo -e "script\tw\th\tpath\tparse_ms\tgen_ms\traster_ms\titems\trendered\toffscreen\toccluded\tskipped\tnonwhite"
for f in $SP/mpcorpus/*.mp; do
  n=$(basename $f .mp)
  for dim in "960 540" "200 200"; do
    set -- $dim
    for p in displaylist displaylist-nomap; do
      o=$(./build/mpharness render "$f" --width $1 --height $2 --path $p --out $SP/out/${n}_$1x$2_$p.pgm 2>&1)
      parse=$(echo "$o" | awk '/^  parse/{print $2}')
      gen=$(echo "$o"   | awk '/^  displaylist/{print $2}')
      ras=$(echo "$o"   | awk '/^  rasterize/{print $2}')
      it=$(echo "$o"    | awk '/display list items/{print $NF}')
      rd=$(echo "$o"    | awk '/rendered items/{print $NF}')
      os=$(echo "$o"    | awk '/culled off-screen/{print $NF}')
      oc=$(echo "$o"    | awk '/culled by occlusion/{print $NF}')
      sk=$(echo "$o"    | awk '/pixels skipped/{print $NF}')
      nw=$(echo "$o"    | awk '/non-white pixels/{print $(NF-3)}')
      echo -e "$n\t$1\t$2\t$p\t$parse\t$gen\t$ras\t$it\t$rd\t$os\t$oc\t$sk\t$nw"
    done
  done
done
