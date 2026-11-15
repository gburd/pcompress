#!/bin/sh
# ci/watchdog.sh <seconds> <command> [args...]
#
# Run one test. If it is still running after <seconds>, print every
# thread's backtrace (gdb, when installed) and kill it with exit status 124.
# A rare hang in a CI unit test otherwise runs until the job limit and
# leaves nothing to debug.
set -u

limit=$1
shift
"$@" &
pid=$!
t=0
while kill -0 "$pid" 2> /dev/null; do
	if [ "$t" -ge "$limit" ]; then
		echo "watchdog: '$*' still running after ${limit}s; backtraces follow" >&2
		if command -v gdb > /dev/null 2>&1; then
			# Yama may forbid tracing a sibling; CI runners allow sudo.
			gdb -batch -p "$pid" -ex 'thread apply all bt' 2>&1 ||
				sudo -n gdb -batch -p "$pid" -ex 'thread apply all bt' 2>&1
		else
			echo "watchdog: no gdb here" >&2
		fi
		kill -9 "$pid"
		wait "$pid" 2> /dev/null
		exit 124
	fi
	sleep 1
	t=$((t + 1))
done
wait "$pid"
