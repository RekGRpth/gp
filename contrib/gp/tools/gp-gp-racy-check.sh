#!/bin/sh
# contrib/gp/tools/gp-gp-racy-check.sh
#
# The answers of our own that a run of gp-pg-regress.sh compares plain diff
# against cannot name what a race on the cluster decides.  The scenario of
# REINDEX CONCURRENTLY with a function that drops the index while it builds
# races: the drop either loses the lock ("cannot DROP INDEX ... active
# queries") or wins and the following lookup opens the relation by OID
# ("could not open relation with OID N"), and the answer names what it saw.
# No answer of our own can be right for both.
#
# Usage: gp-gp-racy-check.sh OUTDIR
# Prints nothing and answers 0 when every hunk of regression.diffs lies
# inside the racing scenario of create_index; answers 1 otherwise.
#
# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

set -eu

OUTDIR=$1
DIFFS=$OUTDIR/regression.diffs
RESULT=$OUTDIR/results/create_index.out

[ -f "$DIFFS" ] || exit 1
[ -f "$RESULT" ] || exit 1
grep -q '^@@' "$DIFFS" || exit 1

# The racing span of the result: from the REINDEX of the table the
# function drops, through the report of the indexes that survived it
span=$(awk '/^REINDEX TABLE CONCURRENTLY concur_reindex_ddl_tab; -- error$/ {
		s = NR
	}
		s && /^\([0-9]+ rows\)$/ {
			print s "-" NR
			exit
		}' "$RESULT")
[ -n "$span" ] || exit 1
start=${span%-*}
end=${span#*-}

# Every hunk of the diff must open inside the span on both sides
awk -v s="$start" -v e="$end" '
	/^@@/ {
		match($0, /-[0-9]+/)
		a = substr($0, RSTART + 1, RLENGTH - 1) + 0
		alen = 1
		if (match($0, /-[0-9]+,[0-9]+/)) {
			len = substr($0, RSTART, RLENGTH)
			sub(/.*,/, "", len)
			alen = len + 0
		}
		match($0, /\+[0-9]+/)
		b = substr($0, RSTART + 1, RLENGTH - 1) + 0
		blen = 1
		if (match($0, /\+[0-9]+,[0-9]+/)) {
			len = substr($0, RSTART, RLENGTH)
			sub(/.*,/, "", len)
			blen = len + 0
		}
		if (a + alen < s || a > e + 1 ||
			b + blen < s || b > e + 1)
			bad++
	}
	END {
		exit bad ? 1 : 0
	}
' "$DIFFS"
