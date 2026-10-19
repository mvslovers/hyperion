#!/bin/bash
# mvsrace.sh <red|green> -- #892 on MVS/CE in a throwaway container.
# red: the image's own Hercules; green: ~/hercules/con1052/inst mounted
# in front of it.  Each round: the burst without pauses, then paced.
set -u; MODE=$1; img=ghcr.io/mvslovers/mvsce:latest; n=hyp-kbq-$MODE
I=${INST:-$HOME/hercules/con1052/inst}   # install prefix of the build under test
extra=(); [ "$MODE" = green ] && extra=(-v $I:$I:ro -e PATH=$I/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin -e LD_LIBRARY_PATH=$I/lib)
docker rm -f $n >/dev/null 2>&1
docker run -d --name $n -p 0:8080 -p 0:8888 "${extra[@]}" "$img" >/dev/null
hp=$(docker port $n 8080/tcp | sed -n "s/^0.0.0.0://p"); cp=$(docker port $n 8888/tcp | sed -n "s/^0.0.0.0://p")
for i in $(seq 1 180); do curl -sf -m 1 -u IBMUSER:SYS1 "http://localhost:$hp/zosmf/test?fn=version" >/dev/null && break; sleep 1; done
sleep 15
docker exec $n sh -c "printf \"/D T\n/D R,L\n/D U,,,190,1\n/D T\n\" > /tmp/burst.rc; printf \"/D T\npause 1\n/D R,L\npause 1\n/D U,,,190,1\npause 1\n/D T\n\" > /tmp/paced.rc"
docker exec $n sh -c "grep -m2 -E \"HHC01413I|HHC01415I\" /logs/hercules.log"
for r in burst paced burst paced burst; do
  m=$(docker exec $n sh -c "wc -l < /logs/hercules.log")
  curl -s -o /dev/null -u hercules:hercules --data-urlencode "command=script /tmp/$r.rc" -d send=Send "http://localhost:$cp/cgi-bin/tasks/syslog"
  sleep 8
  out=$(docker exec $n sh -c "tail -n +$((m+1)) /logs/hercules.log")
  le=$(grep -n HHC00013I <<<"$out" | tail -1 | cut -d: -f1); fe=$(grep -n -E "IEE[0-9]+I" <<<"$out" | head -1 | cut -d: -f1); fl=no; [ -n "$le" ] && [ -n "$fe" ] && [ "$le" -lt "$fe" ] && fl=yes
  echo "== $r: forced=$fl in=$(grep -c HHC00013I <<<"$out") IEE136I=$(grep -c IEE136I <<<"$out") IEE111I=$(grep -c IEE111I <<<"$out") IEE450I=$(grep -c IEE450I <<<"$out") HHC00016W=$(grep -c HHC00016W <<<"$out") prompts=$(grep -c HHC00010A <<<"$out")"
  grep -E "HHC00013I|HHC00016W|HHC00010A|IEE[0-9]+I|IEF|IEA" <<<"$out" | cut -c1-100
done
L=$(docker exec $n sh -c "grep -h KBQDIAG /logs/hercules.log /tk5/log/3033.log 2>/dev/null" | sed -n "s/.*KBQDIAG read \([0-9]*\) us.*/\1/p" | sort -n); echo "KBQDIAG n=$(wc -l <<<"$L") max_us=$(tail -1 <<<"$L") median_us=$(sed -n "$(( ($(wc -l <<<"$L")+1)/2 ))p" <<<"$L")"
docker logs $n 2>docker logs $n 2>&11 | grep -iE "error|cannot|not found" | head -5
docker rm -f $n >/dev/null
