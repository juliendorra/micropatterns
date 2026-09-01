#!/usr/bin/env bash
# Is sweep_city_*.mp still a copy of city.mp's zoom block?
#
# The two probes reproduce a real bug in examples/scripts/city.mp, and they do
# it by carrying that script's zoom block VERBATIM -- the seed construction, the
# constants, the step count, the re-roll. That is the whole reason they are
# trustworthy: a probe that reconstructs the script under test is testing the
# reconstruction. We know that concretely, because a reconstruction WAS tried
# and showed no bug at either modulus while the real block shows four
# unreachable levels.
#
# The weakness of a verbatim copy is that it goes stale silently. The DSL has no
# include, so the copy cannot be eliminated -- but it can be made loud. This
# extracts the block from the source and from each probe and diffs them.
#
# Exits non-zero if they have drifted. If that happens, the right move is to
# re-copy the block into both probes and re-check the expected outcomes; the
# numbers in their headers may no longer hold.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../../examples/scripts/city.mp"

# The block runs from the seed construction to the chosen scaling. Both ends are
# matched on content rather than line number so an edit ABOVE the block does not
# by itself trip this.
START='VAR \$seed = \$HOUR \* 60'
END='VAR \$scaling = \$min_scaling \+ \$scale_pick'

extract() {   # file -> its block, comments and blank lines stripped
    # NOTE: no "\+" in these expressions. BSD sed's basic regex does not treat
    # it as a repetition operator, so 's/[[:space:]]\+/ /g' silently matched a
    # space followed by a LITERAL plus and ate every "+" in the arithmetic --
    # which made this script report drift that was not there, on its first run.
    sed -n "/$START/,/$END/p" "$1" \
        | sed -e 's/#.*//' -e 's/[[:space:]][[:space:]]*/ /g' \
              -e 's/^ //' -e 's/ $//' \
        | grep -v '^$'
}

if [ ! -f "$SRC" ]; then
    echo "check_city_block: source not found: $SRC" >&2
    exit 2
fi

rc=0
for probe in "$HERE/corpus/sweep_city_32749.mp" "$HERE/corpus/sweep_city_32768.mp"; do
    name="$(basename "$probe")"
    # The 32768 variant differs from the source in exactly one way, on purpose:
    # the modulus. Normalise it back before comparing so that is not reported as
    # drift, while any OTHER difference still is.
    if ! diff -u <(extract "$SRC") <(extract "$probe" | sed 's/32768/32749/g') \
            > /tmp/city_block_diff.$$ 2>&1; then
        echo "DRIFT  $name no longer matches examples/scripts/city.mp"
        sed -n '1,40p' /tmp/city_block_diff.$$
        rc=1
    else
        echo "  ok   $name matches examples/scripts/city.mp"
    fi
    rm -f /tmp/city_block_diff.$$
done
exit $rc
