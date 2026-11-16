#!/usr/bin/env bash
# Shows the exchange at work: starts a server, lets two clients trade with each
# other, and prints what each of them sees.
# Usage: scripts/demo.sh [PORT]
set -euo pipefail
cd "$(dirname "$0")/.."

port="${1:-9100}"
bin=build/release/apps

if [[ ! -x $bin/exchange-server || ! -x $bin/exchange-cli ]]; then
    echo "Building the release preset first..."
    cmake --preset release > /dev/null
    cmake --build --preset release > /dev/null
fi

work=$(mktemp -d)
server_pid=""
cleanup() {
    if [[ -n $server_pid ]] && kill -0 "$server_pid" 2> /dev/null; then
        kill -INT "$server_pid"
        wait "$server_pid" || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

# Waits until a file contains a text, for 5 seconds at most.
wait_for() {
    for _ in $(seq 50); do
        if grep -q -- "$2" "$1" 2> /dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "error: timed out waiting for '$2'" >&2
    cat "$1" >&2
    return 1
}

title() { printf '\n\033[1m%s\033[0m\n' "$1"; }

title "1. The server starts"
"$bin/exchange-server" --port "$port" > "$work/server.log" 2>&1 &
server_pid=$!
if ! wait_for "$work/server.log" "listening"; then
    echo "The server did not start. If port $port is in use, try: scripts/demo.sh ANOTHER_PORT" >&2
    exit 1
fi
cat "$work/server.log"

# Alice must stay connected while Bob trades, because leaving cancels her
# orders. Her commands are therefore fed through a pipe that is kept open.
title "2. Alice connects and offers to sell 5 at 101 and 10 at 102"
mkfifo "$work/alice.in"
"$bin/exchange-cli" --port "$port" < "$work/alice.in" > "$work/alice.out" 2>&1 &
alice_pid=$!
exec 3> "$work/alice.in"
printf 'sell 101 5\nsell 102 10\n' >&3
wait_for "$work/alice.out" "order 2 accepted"
cat "$work/alice.out"
alice_lines=$(wc -l < "$work/alice.out")

title "3. Bob connects, looks at the book and buys 8 at up to 102"
echo "   (he gets the best price first: 5 at 101, then 3 at 102)"
printf 'book\nbuy 102 8\nbook\n' | "$bin/exchange-cli" --port "$port"

title "4. Meanwhile, Alice was told about her sales"
wait_for "$work/alice.out" "order 2 filled"
printf 'orders\nquit\n' >&3
exec 3>&-
wait "$alice_pid"
tail -n "+$((alice_lines + 1))" "$work/alice.out"

title "5. Alice has left: the 7 units she still offered were cancelled"
printf 'book\n' | "$bin/exchange-cli" --port "$port"

title "6. The server stops"
kill -INT "$server_pid"
wait "$server_pid" || true
server_pid=""
tail -n 1 "$work/server.log"

printf '\nTo try it yourself, run %s/exchange-server in one terminal and\n%s/exchange-cli in two others.\n' "$bin" "$bin"
