# TODO (mvslovers fork)

Open work on the X'75' TCPIP interface. Not upstream material as it stands --
this file tracks what is known, what is measured, and what is still waiting on
evidence.

## Done

| Change | Fork | Upstream |
|---|---|---|
| SEND must not block the CPU thread | `1a599b0d` | #863 / PR #864, merged |
| BIND sets `SO_REUSEADDR` | `837f2f29` | #869 / PR #870, merged |
| SELECT state dies with its socket | `60dd927e` | #874 / PR #875, open |

## Open: SELECT computes the wrong `nfds`

### What the defect is

`tcpip.c`, `EZASOKET`, `case 17` subcode 4 passes this to the host `select()`:

```c
i = select (Ccom_han [aux2 - 1] + 1, ...);
```

`aux2 - 1` is the highest socket **number** the guest passed -- the guest's
`selectex()` computes `maxsock` that way and hands it in. `Ccom_han [aux2-1]`
is that socket's host **handle**. The two are different things:

- socket numbers are handed out by this module, lowest free slot first
- host handles are handed out by the host, lowest free descriptor first

They start out in step and drift apart as soon as anything that is not a guest
socket opens or closes a descriptor -- a device file, a TN3270 client, the HTTP
console. Once they have drifted, the socket with the highest number is no
longer the socket with the highest handle, and every descriptor in the set at
or above the computed limit is **not examined by `select()`**. It never comes
back ready. Whoever waits on it waits, and which socket it hits is a matter of
timing, which is what would make it intermittent.

The fix is to derive `nfds` from the handles actually in the sets. That is not
the difficult part; the difficult part is that correcting it upward makes
`select()` examine handles it previously skipped, including stale ones, which
turns silent misses into `EBADF` for the whole call. So a correction has to
come together with resolving the guest's socket numbers to host handles when
the select runs, rather than snapshotting handles when the guest hands its
bitmap in. That is a rewrite of five subcodes, which is why it is waiting for
evidence rather than being done.

### Why it has not been fixed

It has no opportunity on the systems we can observe. Measured over 40000 select
runs on MVS 3.8j: `multicalls=0`, `insetmax=1` -- not one call ever had more
than a single descriptor in its set, so the highest number was trivially also
the highest handle and the computation cannot be wrong.

- HTTPD selects on its listener alone: accepted connections go to the worker
  queue, not into the array `build_fd_set()` walks
- FTPD selects on one control or data socket at a time
- ufsd, mvsmf, httplua, httprexx do not select at all

It becomes live the moment a guest multiplexes several sockets in one select.
HTTPD does exactly that in its fallback path, when its worker pool cannot be
created -- so the assumption that protects us today is a consequence of a
configuration succeeding, not of the interface being sound.

### How we will test it

A guest program, not emulator instrumentation: mvslovers/libc370#144,
`test/mvs/tst75sel.c`.

It needs no Hercules change -- no diagnostic build, no install, no IPL -- and
it therefore also works against emulator versions we do not control. It detects
the defect from inside the guest:

1. open a listener on 127.0.0.1 and connect to it N times, so the program owns
   both ends of N pairs and knows every socket number it was given
2. make **every** socket in the set readable -- one byte on each peer
3. `select()` over the whole set
4. cross-check each socket with `ioctlsocket(FIONREAD)`, which is a different
   X'75' function code and does not go through the select path
5. any socket with `FIONREAD > 0` that `select()` did not report is a miss

If every socket in the set is readable, `select()` must report every one of
them. The number missing says how far the limit was off.

Guest-side shuffling alone is not expected to force the divergence: both
allocators take the lowest free entry, so closing and reopening keeps the two
orderings in step as long as the guest is the only actor. The divergence comes
from descriptor churn that is **not** the guest's -- connections opened, held
and closed against the emulator's own listeners (HTTP console port, TN3270
port) while the guest allocates. That is driven from outside the guest and
needs no guest code.

A miss proves reachability and is the evidence the upstream report would
otherwise lack. Zero misses across forced divergence is worth having too: it
bounds how hard the condition is to reach, and after a fix it is the regression
guarantee.

## Also known, not scheduled

Found while reading `tcpip.c`; none of it is measured, none of it is urgent.

- **Unchecked bounds in SELECT.** `aux2` arrives from a guest register and
  indexes `Ccom_han []` without being checked against `Ccom`; the result
  subcodes write the guest's bitmap without checking the length they were
  given. Guest-controlled out-of-bounds read and write. Our guests pass sane
  values.
- **`FD_SET` with a host handle at or above `FD_SETSIZE`** writes past a
  `malloc (sizeof (fd_set))`. Needs roughly a thousand concurrent sockets.
- **`find_slot ()` on exhaustion** stops at `Ccom-1` and assigns anyway, so two
  `talk` structures alias one slot and `map32 [R14]` starts returning the wrong
  session's structure. Reached by leaking talks, which happens when a task dies
  between the two X'75' instructions of one call.
- **`gethostbyname()` / `gethostbyaddr()`** block the CPU thread and are not
  thread-safe -- the same class as the SEND defect, but the fix is a worker
  thread or a cache rather than a flag.
- **`CerrGen`** is a single global across all address spaces, so GETERROR can
  return the error of an unrelated session.
