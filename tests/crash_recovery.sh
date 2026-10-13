#!/bin/bash
# crash_recovery.sh — kill -9 the REPL mid-session and verify WAL recovery.
set -u
cd "$(dirname "$0")/.."

ISQL=./isql
DB=/tmp/crash_test.db.json
WAL="$DB.wal"
FIFO=/tmp/crash_test.fifo
OUT=/tmp/crash_test.out
rm -f "$DB" "$WAL" "$FIFO" "$OUT"
mkfifo "$FIFO"

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; exit 1; }

echo "== starting isql (WAL active), feeding committed statements =="
ISQL_FAKE_LLM=1 ISQL_DB="$DB" "$ISQL" < "$FIFO" > "$OUT" 2>&1 &
PID=$!
exec 3>"$FIFO"                 # hold the fifo open so the REPL keeps waiting
printf 'create a table users with id name age;\n' >&3
sleep 0.4
printf 'insert alice 30;\ninsert bob 25;\ninsert carol 40;\n' >&3
sleep 1                        # every statement fsyncs its WAL entry

WAL_LINES=$(wc -l < "$WAL")
echo "   wal entries before crash: $WAL_LINES"
[ "$WAL_LINES" -ge 4 ] || fail "expected >=4 entries in the log (create + 3 inserts)"

echo "== SIGKILL while the process is idle =="
kill -9 "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
exec 3>&-

echo "== recovering in a fresh process =="
printf 'list all users;\nexit\n' | ISQL_FAKE_LLM=1 ISQL_DB="$DB" "$ISQL" > "$OUT.recover" 2>&1
grep -q "Fetched 3 row(s)" "$OUT.recover" || fail "expected 3 users after recovery"
grep -q '"id":1' "$DB" && grep -q '"id":2' "$DB" && grep -q '"id":3' "$DB" \
  || fail "recovered rows missing (ids 1..3)"
pass "all 3 committed inserts survived kill -9"

echo "== auto-increment continues after recovery =="
printf 'insert dave 22;\nexit\n' | ISQL_FAKE_LLM=1 ISQL_DB="$DB" "$ISQL" > "$OUT.next" 2>&1
grep -q '"id":4' <(cat "$DB") || fail "next auto id should be 4"
pass "next auto-increment id is 4"

echo "== a clean exit checkpoints and empties the WAL =="
[ -s "$WAL" ] && WAL_AFTER=$(wc -l < "$WAL") || WAL_AFTER=0
echo "   wal lines after clean exit: $WAL_AFTER"
[ "$WAL_AFTER" = "0" ] || fail "expected the wal to be truncated after checkpoint"
pass "clean exit truncated the log"

# ---------------------------------------------------------------------------
echo "== open transaction killed by SIGKILL is NOT durable =="
rm -f "$DB" "$WAL" "$FIFO"
mkfifo "$FIFO"
ISQL_FAKE_LLM=1 ISQL_DB="$DB" "$ISQL" < "$FIFO" > "$OUT" 2>&1 &
PID=$!
exec 3>"$FIFO"
printf 'create a table users with id name age;\n' >&3
sleep 0.4
printf 'begin\n' >&3
sleep 0.4
printf 'insert committed_row 10;\n' >&3   # inside the txn -> only buffered
sleep 0.6
kill -9 "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
exec 3>&-

printf 'list everyone;\nexit\n' | ISQL_FAKE_LLM=1 ISQL_DB="$DB" "$ISQL" > "$OUT.rec2" 2>&1
if grep -q "Fetched 0 row(s)" "$OUT.rec2"; then
    echo "   (0 users after recovering from a crashed open txn)"
    pass "uncommitted transaction lost on crash (as required)"
else
    fail "uncommitted transaction should have been rolled back by the crash"
fi

exec 3<&-
rm -f "$DB" "$WAL" "$FIFO" "$OUT" "$OUT.recover" "$OUT.next" "$OUT.rec2"
echo
echo "ALL CRASH-RECOVERY TESTS PASSED"