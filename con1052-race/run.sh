#!/bin/bash
# run.sh <builddir> <N-hex8> <hao cmd> <seconds> <tag>
B=$1; N=$2; CMD=$3; SECS=$4; TAG=$5
cd "$(dirname "$0")"
sed -e "s|@N@|$N|" -e "s|@CMD@|$CMD|" -e "s|@SECS@|$SECS|" race.rc.in > rc.$TAG
( sleep $((SECS+15)) ) | timeout $((SECS+20)) $B/hercules -d -p $B/.libs -f race.cnf -r rc.$TAG > log.$TAG 2>&1
echo "exit=$? prompts=$(grep -c HHC00010A log.$TAG) replies=$(grep -c HHC00013I log.$TAG)"
grep -E "HHC01413I|HHC00809I|HHC00880I|HHC00881I|0:0009|disabled wait|HHCCP|PSW=|psw" log.$TAG | grep -v "HHC00010A\|HHC00013I\|HHC00081I" | head -20
