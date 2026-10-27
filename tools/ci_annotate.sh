#!/usr/bin/env bash
# For CI: runs a command; if it fails, its last lines become the job's error
# annotation (the job's own log isn't always at hand):
#
#   bash tools/ci_annotate.sh "TITLE" COMMAND [ARGS...]
set -uo pipefail
title="$1"
shift
log="$(mktemp)"
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
    lines="$(tail -40 "$log" | sed -e 's/%/%25/g' -e 's/\r//g' | sed -e ':a;N;$!ba;s/\n/%0A/g')"
    echo "::error title=$title::$lines"
fi
rm -f "$log"
exit "$status"
