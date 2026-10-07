#!/bin/bash
#
# Test suite for the elevator group simulation.
# Run from anywhere:  ./tests/run-tests.sh

cd "$(dirname "$0")/.." || exit 1

BIN=build/elevator
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

ok()   { printf "  ok    %s\n" "$1"; pass=$((pass + 1)); }
no()   { printf "  FAIL  %s\n" "$1"; fail=$((fail + 1)); }
check() { if [ "$1" -eq 0 ]; then ok "$2"; else no "$2"; fi; }

run() { "$BIN" --plain --delay-ms 0 "$@"; }

# every passenger delivered and no invariant broken
finished_clean() {
    out=$1
    want=$2
    grep -q "^delivered             $want of $want\$" "$out" &&
    grep -q "^invariants broken     0\$" "$out"
}

echo "building"
make clean >/dev/null 2>&1
if make >"$TMP/build.log" 2>&1; then
    ok "make finishes"
else
    no "make finishes"
    cat "$TMP/build.log"
    exit 1
fi
if grep -qE "warning|error" "$TMP/build.log"; then
    no "no compiler warnings"
    grep -E "warning|error" "$TMP/build.log"
else
    ok "no compiler warnings"
fi

echo
echo "command line"
"$BIN" --help >"$TMP/help.txt" 2>&1
check $? "--help exits with 0"
grep -q "Usage:" "$TMP/help.txt"
check $? "--help prints the usage"

for bad_args in "--floors 99" "--floors 1" "--capacity 0" "--strategy random" \
                "--elevators" "nonsense" "--config /no/such/file"; do
    # shellcheck disable=SC2086
    run $bad_args >/dev/null 2>&1
    if [ $? -eq 1 ]; then ok "rejected: $bad_args"; else no "rejected: $bad_args"; fi
done

echo
echo "scenarios from data/"
for cfg in data/*.conf; do
    name=$(basename "$cfg")
    want=$(sed -n 's/^ *passengers *= *\([0-9]*\).*/\1/p' "$cfg")
    run --config "$cfg" --log "$TMP/$name.log" >"$TMP/$name.out" 2>&1
    if finished_clean "$TMP/$name.out" "$want"; then
        ok "$name: $want passengers delivered, invariants clean"
    else
        no "$name: did not finish cleanly"
        tail -5 "$TMP/$name.out"
    fi
done

# the point of crowded.conf is that cabins fill up, so it must happen
refused=$(sed -n 's/^full cabin refusals *\([0-9]*\)$/\1/p' "$TMP/crowded.conf.out")
[ -n "$refused" ] && [ "$refused" -gt 0 ]
check $? "crowded.conf really fills the cabins ($refused refusals)"

echo
echo "both strategies"
for st in fcfs nearest; do
    run --strategy $st --seed 42 --log "$TMP/$st.log" >"$TMP/$st.out" 2>&1
    if finished_clean "$TMP/$st.out" 25; then
        ok "strategy $st delivers everybody"
    else
        no "strategy $st delivers everybody"
    fi
done

echo
echo "edge cases"
run --passengers 0 --log "$TMP/zero.log" >"$TMP/zero.out" 2>&1
grep -q "^model time            0 ticks\$" "$TMP/zero.out"
check $? "no passengers: stops at once"

run --floors 2 --elevators 8 --capacity 2 --passengers 20 --seed 5 \
    --log "$TMP/flat.log" >"$TMP/flat.out" 2>&1
finished_clean "$TMP/flat.out" 20
check $? "two floors, eight cabins"

run --floors 20 --elevators 1 --capacity 1 --passengers 15 --seed 5 \
    --log "$TMP/one.log" >"$TMP/one.out" 2>&1
finished_clean "$TMP/one.out" 15
check $? "one cabin that holds one person"

run --seed 9 --max-ticks 25 --log "$TMP/cut.log" >"$TMP/cut.out" 2>&1
grep -q "tick limit reached" "$TMP/cut.out" &&
grep -q "requests still open:" "$TMP/cut.out"
check $? "tick limit: reports the open requests"

echo
echo "repeatability"
run --seed 42 --log "$TMP/r1.log" >/dev/null 2>&1
run --seed 42 --log "$TMP/r2.log" >/dev/null 2>&1
grep -v "^run:" "$TMP/r1.log" >"$TMP/r1.cmp"
grep -v "^run:" "$TMP/r2.log" >"$TMP/r2.cmp"
cmp -s "$TMP/r1.cmp" "$TMP/r2.cmp"
check $? "same seed gives the same journal"

run --seed 43 --log "$TMP/r3.log" >/dev/null 2>&1
grep -v "^run:" "$TMP/r3.log" >"$TMP/r3.cmp"
if cmp -s "$TMP/r1.cmp" "$TMP/r3.cmp"; then
    no "another seed gives another journal"
else
    ok "another seed gives another journal"
fi

echo
echo "journal"
run --seed 42 --log "$TMP/j.log" >"$TMP/j.out" 2>&1
grep '^\[' "$TMP/j.out" >"$TMP/j.screen"
grep '^\[' "$TMP/j.log" >"$TMP/j.file"
cmp -s "$TMP/j.screen" "$TMP/j.file"
check $? "events on the screen and in the file are the same"

echo
echo "interruption"
"$BIN" --plain --delay-ms 20 --seed 7 --log "$TMP/int.log" >"$TMP/int.out" 2>&1 &
pid=$!
sleep 1
kill -INT $pid
wait $pid
rc=$?
[ $rc -eq 0 ]
check $? "exits with 0 after SIGINT"
grep -q "interrupted by the user" "$TMP/int.out"
check $? "says it was interrupted"
grep -q "requests still open:" "$TMP/int.out"
check $? "prints the open requests after SIGINT"

echo
echo "random sweep, 60 runs"
sweep_bad=0
for seed in $(seq 1 60); do
    f=$(( seed % 25 + 2 ))
    e=$(( seed % 8 + 1 ))
    c=$(( seed % 6 + 1 ))
    run --floors $f --elevators $e --capacity $c --passengers 25 --seed $seed \
        --log "$TMP/sweep.log" >"$TMP/sweep.out" 2>&1
    if ! finished_clean "$TMP/sweep.out" 25; then
        echo "    broken: floors=$f elevators=$e capacity=$c seed=$seed"
        sweep_bad=$((sweep_bad + 1))
    fi
done
[ $sweep_bad -eq 0 ]
check $? "60 random buildings finish with no broken invariant"

echo
echo "-----------------------------------------"
printf "passed %d, failed %d\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
