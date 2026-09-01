#!/usr/bin/env bash
# The sweep gate.
#
# Two of these cases must FAIL. A plain "run everything and require exit 0" loop
# would report them as broken, so expectations are declared per case rather than
# assumed -- a negative test is only a test if something checks that it still
# goes negative.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MP="$HERE/../build/mpharness"

# name : expected exit : why
CASES=(
  "sweep_city_32749:0:city.mp's real zoom block, prime modulus -- all ten levels reachable"
  "sweep_city_32768:1:same block, power-of-two modulus -- levels 3,4,7,9 unreachable"
  "sweep_bad:1:seed sets only high bits -- every odd level unreachable"
  "sweep_32749:0:reconstructed seed construction, prime -- reaches everything"
  "sweep_32768:0:reconstructed, power-of-two -- ALSO reaches everything, which is the point: a reconstruction does not carry the bug"
)

rc=0
for entry in "${CASES[@]}"; do
    name="${entry%%:*}"; rest="${entry#*:}"
    want="${rest%%:*}"; why="${rest#*:}"
    "$MP" sweep "$HERE/corpus/$name.mp" --levels 10 --width 200 --height 200 >/dev/null 2>&1
    got=$?
    if [ "$got" = "$want" ]; then
        printf "  ok    %-20s exit %s   %s\n" "$name" "$got" "$why"
    else
        printf "  FAIL  %-20s exit %s, wanted %s   %s\n" "$name" "$got" "$want" "$why"
        rc=1
    fi
done

echo
"$HERE/check_city_block.sh" || rc=1
exit $rc
