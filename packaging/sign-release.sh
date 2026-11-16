#!/bin/sh
# packaging/sign-release.sh <file>...
#
# Maintainer only: sign release files with an SSH signing key, writing
# <file>.sig next to each (ssh-keygen -Y sign, namespace "file"). The key
# is taken from git's user.signingkey (the key that signs the tags), or
# from SIGNING_KEY. Verify with packaging/allowed_signers; see
# packaging/README.md.
set -eu

[ $# -gt 0 ] || { echo "usage: $0 <file>..." >&2; exit 2; }
key=${SIGNING_KEY:-$(git config --get user.signingkey || true)}
[ -n "$key" ] || { echo "sign-release.sh: no SIGNING_KEY and no git user.signingkey" >&2; exit 1; }
for f in "$@"; do
	ssh-keygen -Y sign -f "$key" -n file "$f"
done
