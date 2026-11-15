#!/bin/sh
# ci/remote-gate.sh: run ci/gate.sh on test hosts over ssh.
#
#   ci/remote-gate.sh <release-tarball> [previous-release-tarball] host...
#
# For each host: makes a temporary directory under /scratch (GATE_DIR
# overrides), copies the tarball(s) and ci/gate.sh there, runs the gate with
# a low CPU priority, prints its log, and removes the directory. Hosts run
# one after another in the background; the summary waits for all of them.
# Use host names from ~/.ssh/config. Exit status 0 only if every host passed.
set -u
TB=${1:?usage: ci/remote-gate.sh <tarball> [previous-tarball] host...}; shift
PREV=""
case ${1:-} in *.tar.gz) PREV=$1; shift ;; esac
[ $# -gt 0 ] || { echo "no hosts given" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
LOGS=$(mktemp -d "${TMPDIR:-/tmp}/pc-remote-gate.XXXXXX")
SSH="ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30"

run_host() {
	h=$1 log=$LOGS/$1.log
	# The remote login shell may not be sh (fish on some hosts): wrap.
	d=$($SSH "$h" "sh -c 'mkdir -p ${GATE_DIR:-/scratch} && mktemp -d ${GATE_DIR:-/scratch}/pcq.XXXXXX'" < /dev/null) ||
	    { echo "gate: FAIL (cannot reach $h)" > "$log"; return; }
	files="$TB $here/gate.sh"
	[ -n "$PREV" ] && files="$files $PREV"
	# shellcheck disable=SC2086 # word splitting of $files is intended
	if ! scp -q -o BatchMode=yes $files "$h:$d/" < /dev/null; then
		echo "gate: FAIL (copy to $h failed)" > "$log"
	else
		p=""; [ -n "$PREV" ] && p="$d/$(basename "$PREV")"
		$SSH "$h" "sh -c 'cd $d && GATE_DIR=$d nice -n 10 sh gate.sh $d/$(basename "$TB") $p'" \
		    < /dev/null > "$log" 2>&1
	fi
	$SSH "$h" "sh -c 'chmod -R u+w $d 2>/dev/null; find $d -depth -delete'" < /dev/null > /dev/null 2>&1
}

for h in "$@"; do run_host "$h" & done
wait

rc=0
for h in "$@"; do
	echo "===== $h"
	cat "$LOGS/$h.log"
	tail -n 1 "$LOGS/$h.log" | grep -q '^gate: PASS' || rc=1
done
echo "===== summary"
for h in "$@"; do printf '%-10s %s\n' "$h" "$(tail -n 1 "$LOGS/$h.log")"; done
find "$LOGS" -depth -delete
exit $rc
