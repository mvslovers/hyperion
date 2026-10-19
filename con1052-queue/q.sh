#!/bin/bash
# q.sh <builddir> <tag>
B=$1; TAG=$2
cd "$(dirname "$0")"
( sleep 8 ) | timeout 12 $B/hercules -d -p $B/.libs -f q.cnf -r q.rc > qlog.$TAG 2>&1
grep -E "HHC01413I|HHC00013I|HHC00016W|HHC00010A|^R:0000|^R:000004|^R:0000080|^R:0000088|^R:0000090|R:000005|IOPENDING|pending|0:0009" qlog.$TAG | grep -v "HHC00081I"
