# TODO (mvslovers fork)

Open work on the X'75' TCPIP interface. Not upstream material as it stands --
this file tracks what is known, what is measured, and what is still waiting on
evidence.

## Done

| Change | Fork | Upstream |
|---|---|---|
| SEND must not block the CPU thread | `1a599b0d` | #863 / PR #864, merged |
| BIND sets `SO_REUSEADDR` | `837f2f29` | #869 / PR #870, merged |
| SELECT state dies with its socket | `60dd927e` | #874 / PR #875, merged |

The build on `mvsdev` is `4.10.0.11739-SDL-DEV-g60dd927e`, dated 25 Aug 2026.
It carries everything in that table and nothing below it -- in particular not
the restart fix, which has never been compiled or run anywhere.

## Open: a restarted copy replays the host buffer from its start

Fix written and committed on the fork, not yet built, measured or proposed
upstream:

| Branch | Commit | What it is |
|---|---|---|
| `fix/x75-restart-resume` | `fcf7d15d` | the fix alone, the form to propose upstream |
| `diag/x75-restart-trace` | `f1f1f9d1` | red: instrumentation and `lar_offset ()`, no fix |
| `diag/x75-restart-trace` | `392c22c6` | green: the same, one line further |

### What the defect is

`x75.c` copies between the guest buffer and the host buffer in 256-byte
segments:

```c
if (regs->GR_L(1) != 0) s = (unsigned char *)(map32[regs->GR_L(2)]);

while (regs->GR_L(1) != 0) {
    i = regs->GR_L(1) - 1;
    if (i > 255) i = 255;
    ...
    effective_addr2 += i;
    (regs->GR_L(b2)) += i; /* Exception, can recalculate if/when restart */
    s += i;                /* Next PC byte segment location */
    (regs->GR_L(1)) -= i;
}
```

The instruction is restartable by design. A page fault on the guest buffer is a
nullifying exception -- `cpu.c` backs the PSW up by `ilc` for
`PGM_PAGE_TRANSLATION_EXCEPTION` -- so MVS resolves the page and the instruction
runs again from the top. R0 records that the native call has already been made,
R1 holds the bytes still to copy, and the base register has been advanced. The
**guest** side of the copy therefore resumes exactly where it stopped.

The **host** side does not. `s` is a local, recomputed from `map32 [R2]` on every
entry, and R2 is a slot index into `map32 []` that is never advanced. After a
restart the remaining bytes are copied from the **start** of the host buffer to
the already advanced guest address.

For RECV (`R3 = 1`) that is a replay into the guest: a 636-byte message whose
copy faults on its second segment lands as `M[0..255]` followed by `M[0..379]`.
For SEND (`R3 = 0`) it is the mirror image -- the host buffer loses its leading
segments and receives the tail shifted down over them.

The comment on the base-register line shows that the author designed for the
restart. One side of the copy was covered.

Note what does *not* have to go wrong for this: `vstorec` resolves both page
addresses through `MADDRL` before either `memcpy`, so a segment is atomic
against a translation exception. A fault on the very first segment restarts
having copied nothing, which is correct. The defect needs at least one
**completed** segment, i.e. a transfer longer than 256 bytes.

### Why it has been read as a stack defect for years

Because every workaround for it has been a length cap, and a length cap looks
like it works.

- libc370 `src/dyn75/@@75recv.c` has capped `recv()` at 4096 since December
  2024, with the comment "the buf is overwritten from the start after 4096
  bytes are received".
- mvslovers/mvsmf then hit the same corruption at >2048 **with that cap in
  place**, capped its own reads at 2048 (`d2783f5`), and that failed five days
  later (`4bc1014`, byte-at-a-time).

Three caps -- 4096, 2048, 1 -- each of which held until it did not. Only a
probabilistic cause explains that, and the probability here is whether a guest
page is resident when the copy crosses into it. A larger transfer touches more
pages, so it faults more often; that is the whole of the size correlation, and
it is why no cap above 256 can be a fix.

An independent sighting corroborates it. `twinslow/mvs_nfsd`, `socktest/` --
a different application (NFSv3) using the same X'75' socket layer -- reports
exactly `M[0..255]` followed by `M[0..379]` for a 636-byte message, first bad
word at offset 256 reading offset 0, 95 bad words. Every number is what this
mechanism predicts. Two details of that report point away from the "a partial
`recv()` does not consume" reading given there: the message is stated to have
arrived as a **single TCP fragment**, which leaves no reason for a 256-byte
partial read; and 256 is not a TCP or socket-buffer quantity, it is
`if (i > 255) i = 255` in this file.

That report also records the workload dependence: the corruption appeared in a
request/response server that sits in `select()` and does slow disk I/O between
reads, and never in a one-way receive loop. That is the residency argument
stated from the other end -- a buffer that has been idle across an I/O wait is
a buffer whose pages can have been stolen.

### What the guest stub actually encodes

Two things had to be read off the guest side before any fix could be judged,
and both are settled. libc370 `src/dyn75/@@75.s`:

```
         LA    3,0              To Host PC
         SLR   0,0              Restart = No
         DC    X'75005000'      TCPIP 0,000(0,R5)
...
         LA    3,1              From Host PC
         SLR   0,0              Restart = No
         DC    X'75006000'      TCPIP 0,000(0,R6)
```

1. `SLR 0,0` before each of the two instructions, so R0 is zero on entry and
   `R0 != 0` on entry really is a restart and nothing else.
2. The B field is **5** and **6**, never 0. That matters because
   `(regs->GR_L(b2)) += i` is unconditional: with B=0 it would advance R0, and
   any fix that gives R0 a numeric meaning would then advance it twice per
   segment. It does not arise. crent370's `dyn75/@@75.s` is byte-for-byte the
   same stub.

A third issuer exists and was **not** read: Shelby Beach's EZASMI backend
`EZASOH03`, which is not in any checkout here. `nsf370`'s ADR-0029 records
`DC X'75005000'` at its line 1139, so its B field agrees, but whether it also
zeroes R0 is unverified.

`STM 0,15,0(11)` writes every register back into the guest's `PL75`, so R0's
final value is visible to the guest as `pl.r0`. `__75.h` documents that field as
"0 (Initially, but turns to > 0 after call", and no caller in `src/dyn75/`
reads it.

### The fix, and why it cannot touch the guest

The host side needs a resume point. Rather than invent state to carry it, note
that it is already **derivable**: the bytes copied so far are the length this
conversation was given minus what R1 still says is outstanding, and both
lengths live in the `talk` structure that `map32 [R14]` already resolves.
`lar_offset ()` in `tcpip.c` returns that difference, and `x75.c` adds it to
the buffer base:

```c
    if (regs->GR_L(1) != 0)
        s = (unsigned char *)(map32[regs->GR_L(2)]) + lar_offset (&(regs->gr [0]));
```

```c
u_int  lar_offset (u_int  * regs) {
    talk_ptr t = (talk_ptr)map32[get_reg (regs, 14)];
    u_int    len  = (get_reg (regs, 3) == 0) ? t->len_in : t->len_out;
    u_int    left = get_reg (regs, 1);

    if (left >= len) return (0); /* First entry: nothing copied yet */
    return (len - left);
}
```

That `len` is the right one to subtract from was read off `lar_tcpip ()` rather
than assumed, because the arithmetic is added to a host heap pointer and a wrong
`len` is a wild write rather than a replay. Both arms hold:

- `R3 = 0`: the initial call sets `t->len_in = get_reg (regs, 1)`, so `len_in`
  **is** the R1 the copy starts with.
- `R3 = 1`: the initial call sets `set_reg (regs, 1, t->len_out)`, so R1 is
  loaded **from** `len_out`.

R1 therefore starts each copy exactly equal to the length it is compared
against; `left > len` cannot arise, and the first entry falls out as `0` rather
than needing to be special-cased. Neither field is written again while a copy is
in progress -- `len_in` has exactly one assignment in the whole file, and
`len_out` is set by `EZASOKET` before R1 is loaded from it. `t->len_out = 0` at
talk creation covers the remaining worry: the structure is `malloc`ed without
zeroing, but this field never holds garbage, and a zero `len` yields offset 0.

Derived state cannot fall out of step with the copy the way a second counter
can, and there is nothing to initialise -- which matters here, because a `talk`
is reused by the second instruction of the pair, so a tracked counter would have
needed resetting in both arms of `lar_tcpip ()`'s initial call.

The alternative considered and rejected was widening R0 from a flag to a
counter of `1 + bytes copied`. It is a three-line diff and the two guest stubs
we can read allow it -- `SLR 0,0`, nobody reads `pl.r0`. Three reasons not to,
in order of weight:

1. **The issuer we cannot read.** `EZASOH03` is not inspectable from here, and
   any R0 change is a bet on what it does. `lar_offset ()` leaves R0 exactly
   the flag it is today, so no issuer -- read or unread -- can be broken by it.
2. It would make `pl.r0` vary with the transfer length. Guest-visible state
   this fork has no reason to disturb.
3. It would take a guest register and add it to a host heap pointer, where
   today a fabricated R0 costs only a skipped `lar_tcpip ()`. `lar_offset ()`
   dereferences `map32 [R14]`, which `lar_tcpip ()` already does on every
   non-restart entry -- on a restart it is a read that did not happen before,
   but R14 holds a slot the emulator itself wrote and the guest never supplied.

The compatibility consequence is worth stating explicitly, because it decides
what the guests have to keep doing: **a guest cannot detect this fix.** There is
no return value, status bit or function code that distinguishes a patched
emulator, and adding one would change the interface for every existing guest.
So no guest may drop its workaround on the strength of this change, and the
correct guest-side cap -- 256, not 4096 -- is permanent regardless
(mvslovers/libc370, `@@75recv.c`).

### How we will test it

From the emulator, not the guest, and it is a counter rather than a reproducer.
The guest issues `SLR 0,0` before every X'75' and only this instruction ever
sets R0 non-zero, so **entry to `DEF_INST( tcpip )` with `GR_L(0) != 0` is
exactly a restart**. Log R0, R1 and R3 there: a restart with R1 below the
original transfer length is a corruption that has just happened, not one
inferred.

The instrumentation has to stay in place across both sides of the change,
because the result that confirms the fix is **restarts still happening, with no
corruption**. Restarts disappearing would mean the fault rate moved, not the
resume path, and would say nothing.

Which guest to run it under is not free either. A guest capped at or below 256
bytes per call can never complete a segment and so can never show the defect --
mvsmf's byte-at-a-time read (`4bc1014`) is immune by accident. libc370's 4096
cap is not: sixteen segments per call, fifteen of them able to fault after a
completed one. Anything built on current libc370 is a usable subject.

Better than waiting for a fault is forcing one, which is what the red/green
probe does.

**Emulator side**: `diag/x75-restart-trace` carries the instrumentation with
`X75_TRACE_RESTART` already active -- it is a measuring branch, so the define is
not left commented out on it. It logs every entry with `GR_L(0) != 0`, together
with `lar_offset ()` -- the bytes the nullified copy had already moved.
`done > 0` is a transfer an unfixed build is about to replay, and the running
total of those is the second number in the log line:

```
X75 restart 7 (3 after a completed segment): dir=1 left=512 done=512 talk=4
```

Two builds are needed and they are the two commits on that branch, which differ
by **one line**:

| | Commit | `X75_TRACE_RESTART` | `+ lar_offset (...)` in `x75.c` |
|---|---|---|---|
| red | `f1f1f9d1` | defined | out |
| green | `392c22c6` | defined | in |

So: build `HEAD~1`, measure, `git checkout` `HEAD`, build, measure. Nothing is
edited by hand between the two runs, which is the point of having them as
commits -- the red build still has to compile `lar_offset ()`, because the trace
calls it, and reverting `x75.c` wholesale would take the trace out along with
the fix and produce a run that measures nothing.

Note that `fix/x75-restart-resume` is **not** one of the two. It is the same
line without the instrumentation, for proposing upstream, and a run of it
measures nothing either.

**Guest side**: mvslovers/libc370, `test/mvs/tst75rst.c` and `jcl/tst75rst.jcl`.
It places a page boundary at a chosen multiple of 256 inside a 1024-byte recv
buffer and makes the page beyond it non-resident immediately before the recv --
by `PGRLSE` (SVC 112), and in a second case simply by never referencing it. The
segments below the boundary complete, the one at it faults, and an unfixed
emulator copies the remainder from `host[0]`. The recv goes through `__75 ()`
rather than `recv ()` so that one measurement is one pair of instructions, with
no retry loop to blur it. The byte pattern is `i % 251`: any period dividing
256 would make a replay starting at a multiple of 256 invisible.

The two sides are needed together. R0 comes back as `1` either way, so the
guest cannot see a restart, and a clean run has two causes -- fixed, or never
faulted. Only the emulator log separates them, and the number that separates
them is the *second* one: restarts **after a completed segment**. A run whose
restarts all report `done=0` faulted on segment 0 every time, which is correct
behaviour on both builds and proves nothing. The green run is evidence when
that count is above zero and comparable to the red run's.

The guest-side prediction is sharper and needs no emulator build, which makes it
the one to send to `twinslow/mvs_nfsd`: their `rxtest.c` already records
`requested`/`returned`/`at` per `recv()` call, so

1. a corruption whose trace shows a **single** call (636 requested, 636
   returned) confirms a source-side restart and rules out a second `recv()`
   having replayed;
2. `first_bad` is **always a multiple of 256**, because the fault can only land
   on a segment boundary. A TCP-boundary cause predicts arbitrary offsets, so a
   few dozen hits settle it either way.

Their `sender.py` fragments the TCP stream to provoke partial reads, which under
this mechanism is the wrong variable; the `-r` and `-s` options (reply between
reads, `select()` first) are the ones that matter, because they are what let the
receive buffer go cold.

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
