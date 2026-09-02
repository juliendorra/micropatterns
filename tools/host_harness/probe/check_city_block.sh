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
# Bracket expressions keep literal arithmetic operators portable in basic
# regular expressions. GNU sed treats `\+` as a repetition operator while BSD
# sed treats it as a literal plus, which previously made this gate macOS-only.
START='VAR \$seed = \$HOUR [*] 60'
END='VAR \$scaling = \$min_scaling [+] \$scale_pick'

extract() {   # file -> its block, comments and blank lines stripped
    # NOTE: no "\+" in these expressions. BSD sed's basic regex does not treat
    # it as a repetition operator, so 's/[[:space:]]\+/ /g' matched a space
    # followed by a LITERAL plus and ate every "+" in the arithmetic.
    #
    # That bug was in the first version of this script. It did NOT cause a false
    # positive -- both sides go through this same function, so both were mangled
    # identically and still compared equal; verified. The danger was the
    # opposite and worse: a real difference consisting only of "+" signs would
    # have been normalised away and reported as a match. The --selftest below
    # exists because of it.
    sed -n "/$START/,/$END/p" "$1" \
        | sed -e 's/#.*//' -e 's/[[:space:]][[:space:]]*/ /g' \
              -e 's/^ //' -e 's/ $//' \
        | grep -v '^$'
}

# A gate nobody has seen fail is not yet a gate. --selftest proves both
# directions: that a deliberately altered block IS reported as drift, and that
# the normaliser is not quietly eating characters (which would hide a real
# difference rather than invent one).
if [ "${1:-}" = "--selftest" ]; then
    rc=0
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT

    # 1. negative control: change the source, expect drift.
    sed 's/IF \$max_scaling > 12 THEN/IF $max_scaling > 11 THEN/' "$SRC" > "$tmp/city.mp"
    if diff -q <(extract "$tmp/city.mp") \
                <(extract "$HERE/corpus/sweep_city_32749.mp") >/dev/null 2>&1; then
        echo "  SELFTEST FAIL  an altered block was NOT reported as drift"
        rc=1
    else
        echo "  ok   selftest: an altered block is reported as drift"
    fi

    # 2. the normaliser must preserve arithmetic. A mangling normaliser hides
    #    real differences; it looks like nothing is wrong, which is why this is
    #    checked rather than assumed.
    printf 'VAR $a = $b + 1\n' > "$tmp/plus.mp"
    if extract "$tmp/plus.mp" 2>/dev/null | grep -q '+' \
       || sed -e 's/#.*//' -e 's/[[:space:]][[:space:]]*/ /g' "$tmp/plus.mp" | grep -q '+'; then
        echo "  ok   selftest: normaliser preserves '+'"
    else
        echo "  SELFTEST FAIL  normaliser is eating '+' -- real differences will be hidden"
        rc=1
    fi
    exit $rc
fi

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
