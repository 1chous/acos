#!/bin/bash
#
# Test suite for the multithreaded elevator group simulation.
# Run from anywhere:  ./tests/run-tests.sh
#
# Everything is checked twice: once for the build on semaphores and once
# for the build on condition variables.

cd "$(dirname "$0")/.." || exit 1

SEM=build/elevator-sem
COND=build/elevator-cond
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

ok()    { printf "  ok    %s\n" "$1"; pass=$((pass + 1)); }
no()    { printf "  FAIL  %s\n" "$1"; fail=$((fail + 1)); }
check() { if [ "$1" -eq 0 ]; then ok "$2"; else no "$2"; fi; }

# macOS has no timeout(1): a hang is the main kind of failure here, so
# every run gets a watchdog that kills it.
run_to() {
    local limit=$1 bin=$2
    shift 2
    "$bin" --plain --refresh-ms 20 "$@" >"$TMP/out.txt" 2>&1 &
    local pid=$!
    ( sleep "$limit"; kill -9 $pid 2>/dev/null ) &
    local wd=$!
    disown $wd 2>/dev/null
    wait $pid
    local rc=$?
    kill $wd 2>/dev/null
    return $rc
}

delivered_all() {
    grep -q "^delivered  *$1 of $1\$" "$TMP/out.txt" &&
    grep -q "^invariants broken     0\$" "$TMP/out.txt"
}

echo "building"
make clean >/dev/null 2>&1
if make >"$TMP/build.log" 2>&1; then
    ok "make builds both binaries"
else
    no "make builds both binaries"
    cat "$TMP/build.log"
    exit 1
fi
if grep -qE "warning|error" "$TMP/build.log"; then
    no "no compiler warnings"
    grep -E "warning|error" "$TMP/build.log"
else
    ok "no compiler warnings"
fi
[ -x "$SEM" ] && [ -x "$COND" ]
check $? "both executables exist"

echo
echo "command line"
"$SEM" --help >"$TMP/help.txt" 2>&1
check $? "--help exits with 0"
grep -q "Usage:" "$TMP/help.txt"
check $? "--help prints the usage"

for bad_args in "--floors 99" "--floors 1" "--capacity 0" "--strategy random" \
                "--elevators" "nonsense" "--config /no/such/file"; do
    # shellcheck disable=SC2086
    run_to 10 "$SEM" $bad_args
    if [ $? -eq 1 ]; then ok "rejected: $bad_args"; else no "rejected: $bad_args"; fi
done

echo
echo "scenarios from data/, both builds"
for bin in "$SEM" "$COND"; do
    name=$(basename "$bin")
    for cfg in data/*.conf; do
        want=$(sed -n 's/^ *passengers *= *\([0-9]*\).*/\1/p' "$cfg")
        run_to 90 "$bin" --config "$cfg" --log "$TMP/x.log" \
               --travel-ms 10 --door-ms 10 --board-ms 5 --alight-ms 5 \
               --arrival-min-ms 0 --arrival-max-ms 30
        if delivered_all "$want"; then
            ok "$name $(basename "$cfg"): $want delivered, invariants clean"
        else
            no "$name $(basename "$cfg")"
            tail -4 "$TMP/out.txt"
        fi
    done
done

# crowded.conf exists to fill the cabins up, so that must really happen
run_to 90 "$SEM" --config data/crowded.conf --log "$TMP/x.log" \
       --travel-ms 10 --door-ms 10 --board-ms 5 --alight-ms 5 \
       --arrival-min-ms 0 --arrival-max-ms 5
refused=$(sed -n 's/^full cabin refusals  *\([0-9]*\)$/\1/p' "$TMP/out.txt")
[ -n "$refused" ] && [ "$refused" -gt 0 ]
check $? "crowded.conf really fills the cabins ($refused refusals)"

echo
echo "both strategies"
for st in fcfs nearest; do
    run_to 60 "$SEM" --strategy $st --passengers 20 --seed 42 \
           --travel-ms 10 --door-ms 10 --board-ms 5 --alight-ms 5 \
           --arrival-min-ms 0 --arrival-max-ms 30
    delivered_all 20
    check $? "strategy $st delivers everybody"
done

echo
echo "edge cases"
run_to 20 "$SEM" --passengers 0
grep -q "all passengers delivered" "$TMP/out.txt"
check $? "no passengers: stops at once"

run_to 60 "$SEM" --floors 2 --elevators 8 --capacity 2 --passengers 20 --seed 5 \
       --travel-ms 5 --door-ms 5 --board-ms 3 --alight-ms 3 \
       --arrival-min-ms 0 --arrival-max-ms 20
delivered_all 20
check $? "two floors, eight cabins"

run_to 60 "$SEM" --floors 20 --elevators 1 --capacity 1 --passengers 12 --seed 5 \
       --travel-ms 5 --door-ms 5 --board-ms 3 --alight-ms 3 \
       --arrival-min-ms 0 --arrival-max-ms 20
delivered_all 12
check $? "one cabin that holds one person"

run_to 30 "$SEM" --passengers 50 --max-seconds 2 --travel-ms 200
grep -q "time limit reached" "$TMP/out.txt" && grep -q "requests still open:" "$TMP/out.txt"
check $? "time limit: reports the open requests"

echo
echo "journal"
run_to 60 "$SEM" --passengers 8 --seed 3 --travel-ms 20 --door-ms 20 \
       --arrival-min-ms 0 --arrival-max-ms 50 --log "$TMP/j.log"
grep '^\[' "$TMP/out.txt" >"$TMP/j.screen"
grep '^\[' "$TMP/j.log" >"$TMP/j.file" 2>/dev/null
lines=$(wc -l <"$TMP/j.screen")
[ "$lines" -gt 10 ] && cmp -s "$TMP/j.screen" "$TMP/j.file"
check $? "journal matches the screen, $lines events"

echo
echo "signals, both builds"
for bin in "$SEM" "$COND"; do
    name=$(basename "$bin")

    "$bin" --plain --passengers 60 --seed 5 --travel-ms 100 --door-ms 100 \
           --arrival-min-ms 100 --arrival-max-ms 300 --log "$TMP/s.log" \
           >"$TMP/int.txt" 2>&1 &
    pid=$!
    sleep 2
    kill -INT $pid
    wait $pid
    rc=$?
    [ $rc -eq 0 ] && grep -q "interrupted by the user" "$TMP/int.txt" &&
        grep -q "requests still open:" "$TMP/int.txt"
    check $? "$name: SIGINT stops it and prints the open requests"

    "$bin" --plain --passengers 60 --seed 3 --travel-ms 60 --door-ms 60 \
           --arrival-min-ms 80 --arrival-max-ms 200 --log "$TMP/s.log" \
           >"$TMP/usr.txt" 2>&1 &
    pid=$!
    sleep 2
    kill -USR1 $pid
    ( sleep 40; kill -9 $pid 2>/dev/null ) &
    wd=$!
    disown $wd 2>/dev/null
    wait $pid
    kill $wd 2>/dev/null
    after=$(awk '/SIGUSR1/{f=1} f&&/appears/{c++} END{print c+0}' "$TMP/usr.txt")
    grep -q "stopped by SIGUSR1" "$TMP/usr.txt" && [ "$after" -eq 0 ]
    check $? "$name: SIGUSR1 stops new passengers and drains the building"
done

echo
echo "random sweep, 30 runs per build"
for bin in "$SEM" "$COND"; do
    name=$(basename "$bin")
    sweep_bad=0
    for seed in $(seq 1 30); do
        f=$(( seed % 14 + 2 ))
        e=$(( seed % 5 + 1 ))
        c=$(( seed % 5 + 1 ))
        run_to 30 "$bin" --floors $f --elevators $e --capacity $c \
               --passengers 15 --seed $seed --travel-ms 5 --door-ms 5 \
               --board-ms 3 --alight-ms 3 --arrival-min-ms 0 --arrival-max-ms 20
        if ! delivered_all 15; then
            echo "    broken: floors=$f elevators=$e capacity=$c seed=$seed"
            sweep_bad=$((sweep_bad + 1))
        fi
    done
    [ $sweep_bad -eq 0 ]
    check $? "$name: 30 random buildings, no hang and no broken invariant"
done

echo
echo "race detector"
if cc -std=c17 -D_POSIX_C_SOURCE=200809L -g -fsanitize=thread -pthread \
      -o "$TMP/tsan-sem" src/config.c src/sim.c src/output.c src/main.c \
      src/sync_sem.c 2>/dev/null &&
   cc -std=c17 -D_POSIX_C_SOURCE=200809L -g -fsanitize=thread -pthread -DUSE_COND \
      -o "$TMP/tsan-cond" src/config.c src/sim.c src/output.c src/main.c \
      src/sync_cond.c 2>/dev/null; then
    for t in "$TMP/tsan-sem" "$TMP/tsan-cond"; do
        run_to 90 "$t" --floors 8 --elevators 3 --capacity 2 --passengers 15 \
               --seed 7 --travel-ms 15 --door-ms 15 --board-ms 8 --alight-ms 8 \
               --arrival-min-ms 5 --arrival-max-ms 50
        races=$(grep -c "WARNING: ThreadSanitizer" "$TMP/out.txt")
        [ "$races" -eq 0 ]
        check $? "$(basename "$t"): ThreadSanitizer found no data races"
    done
else
    echo "  skip  ThreadSanitizer is not available in this compiler"
fi

echo
echo "-----------------------------------------"
printf "passed %d, failed %d\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
