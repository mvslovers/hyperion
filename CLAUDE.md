# CLAUDE.md — mvslovers/hyperion

Fork of SDL-Hercules-390/hyperion. The work here is the **X'75' TCPIP
instruction** (`x75.c`, `tcpip.c`, `tcpip.h`, `x75.h`) — the emulator side of
the socket interface the mvslovers MVS 3.8j ecosystem runs on.

`TODO.md` is the running record of what is known, measured, and still open.
Read it before touching X'75'; it carries the reasoning, not just the verdicts.

---

## Scope and remotes

**Work only in `mvslovers/hyperion`.** Never push to
`SDL-Hercules-390/hyperion` — upstream gets pull requests, opened deliberately,
never a push.

Both checkouts now name the fork `origin`:

| | `origin` | other |
|---|---|---|
| Mac `~/repos/hyperion` | `mvslovers/hyperion` | `upstream` = SDL (fetch only) |
| `mvsdev:~/hercules/hyperion` | `mvslovers/hyperion` | none — SDL was removed |

The SDL remote was deliberately deleted on mvsdev (3 Sep 2026) because it used
to be `origin` there, which made a bare `git push origin` point at upstream.
**Do not add it back**; the build host does not need it, and syncing with
upstream belongs on the Mac.

## Repo conventions

- **One topic per branch and per PR.** Never bundle unrelated fixes.
  `fix/x75-<slug>` off `develop` for a change to propose upstream,
  `diag/x75-<slug>` for instrumentation that is not for merging.
- Commit subjects name the file: `tcpip.c: let SELECT state die with the socket
  it is keyed to`. Diagnostic commits say so: `x75.c: DIAGNOSTIC BUILD -- …`.
- Commit bodies carry the *why* and the evidence — measured numbers, job ids,
  what was ruled out. `git log` is where the reasoning lives.
- **Nothing AI-related** in commits, comments, PRs or issues. Strip any
  `Claude-Session` trailer.
- X'75' compatibility runs **both ways**: old guests on a new emulator and new
  guests on an old one must both keep working. A guest cannot detect a patched
  emulator — there is no return code, status bit or function code that
  distinguishes one — so no guest may drop a workaround because a fix landed
  here.

---

## Building on mvsdev

`mvsdev` (ssh alias; `mvsdev.lan`, 192.168.0.233) is the build and run host.
Checkout `~/hercules/hyperion`, autotools build directory
`~/hercules/hyperion/build` with `srcdir = ..`, so the checked-out branch is
what gets built.

```sh
ssh mvsdev
cd ~/hercules/hyperion
git fetch origin && git checkout <branch>
touch version.c                  # ALWAYS — see below
cd build && make -j4
```

**`touch version.c` is not optional.** Without it an incremental build keeps
the previous commit hash and date in the version banner, and a binary that
lies about which commit it is destroys a red/green comparison — an accidental
run of the old build looks exactly like a successful red run.

`cc370`, `as370`, `ld370`, `ar370` are installed in `~/.local/bin` on both
machines (`PREFIX ?= $(HOME)/.local`, no sudo). They are not on a
non-interactive `PATH`; export it.

### Installing — this step is Mike's

`~/MVSCE/start_mvs.sh` calls a bare `hercules`, which resolves to
`/usr/local/hercules/bin/hercules` (root-owned). So a build in the build tree
changes nothing until it is installed:

```sh
cd ~/hercules/hyperion/build && sudo make install
```

**Ask Mike to run it**; do not try to. Then MVSCE has to be stopped and
restarted for the new binary to take effect.

### Verify what is actually running — every time

```sh
ssh mvsdev 'grep -m2 -E "HHC01413I|HHC01415I" ~/MVSCE/hercules.log'
```

The log is truncated at each start, so its head names the running build. Check
this after every install, before every measurement. A mislabelled binary is the
one failure that silently invalidates a whole experiment.

---

## MVSCE

`~/MVSCE` on mvsdev, MVS 3.8j. `bash start_mvs.sh` — Hercules runs in a tmux
pane; MVS console commands go into it with a **leading slash** (`/d a,l`,
`/p ftpd`, `/c httpd`). Neither the `.` prefix nor `scpimply` works: both route
to SCLP, which MVS 3.8j does not use. The Hercules web console on `:8181`
reaches Hercules only, not MVS.

- Hercules log: `~/MVSCE/hercules.log` — truncated on each start, carries both
  emulator messages and the MVS console (WTOs included).
- TSO: tn3270 on `:3270`. Users `IBMUSER/SYS1`, `MVSCE01/CUL8TR`,
  `MVSCE02/PASS4U`.
- Card readers: `000C` → `localhost:3505` (ASCII), `001A` → `localhost:3506`
  (EBCDIC). Printers `000E`/`000F` → `~/MVSCE/printers/`.
- HTTPD on `:8080`, and **mvsmf is served behind it** — the z/OSMF-compatible
  REST API is `http://mvsdev.lan:8080/zosmf/…`, not a separate port.

---

## Deploying and running an MVS probe

Driven from the Mac with `zowe` (profile `mvsdev` → `mvsdev.lan:8080`, user
`ibmuser`). libc370 is the cc370 sysroot, **not an mbt project**, so it has no
`make deploy` — probes are built and shipped by hand. Working copy is
`~/repos/mvs/libc370`.

```sh
L=~/repos/mvs/libc370
cc370 -I$L/include $L/test/mvs/tstXXX.c -flinker-output=iebcopy -o TSTXXX
ld370 --pack TSTXXX.iebcopy -o probe -xmit --dsn IBMUSER.LIBC370.<SCRATCH>

# staging data set: CHECK EVERY TIME, RECEIVE sometimes consumes it
zowe files list ds "IBMUSER.MBT.XMIT.IN" ||
zowe files create ps "IBMUSER.MBT.XMIT.IN" --record-format FB \
     --record-length 80 --block-size 3120 --size 10CYL --secondary-space 5
zowe files upload ftds probe.xmit "IBMUSER.MBT.XMIT.IN" --binary

zowe jobs submit lf recv.jcl --wait-for-output --directory out
zowe jobs submit lf $L/jcl/tstXXX.jcl --wait-for-output --directory out
```

`--directory out` writes the spool to `out/<jobid>/<step>/SYSPRINT.txt`, and
`out/<jobid>/JES2/JESMSGLG.txt` is the job log — **which carries the WTOs the
job issued**, so "did this write to the console?" is answerable from a plain
submit with no SYSLOG access.

### Traps, each of which cost a cycle

- **No `USER=`/`PASSWORD=` on the JOB card.** mvsmf appends its own
  `NOTIFY=$MVSMF,USER=…,PASSWORD=` continuation; yours collides with it and the
  job dies before step 1 with `IEF652I MUTUALLY EXCLUSIVE KEYWORDS`. (Jobs fed
  through the *card reader* do need them — that is a different path.)
- **`MSGCLASS=H`, never `A`.** Class A prints and JES2 purges the job
  immediately, so `zowe` finds nothing to download and reports "Zero jobs were
  returned" for a job that ran fine. Every `jcl/*.jcl` in libc370 uses H.
- **RECEIVE will not merge into an existing PDS.** `IBMUSER.LIBC370.PROBE.LINKLIB`
  holds other probes and is full. Do not delete it. RECEIVE into a scratch PDS
  — build the XMIT with `--dsn <SCRATCH>` so RECEIVE targets it — then one
  IEBCOPY step adds the member:

  ```
  //IN   DD DSN=IBMUSER.LIBC370.<SCRATCH>,DISP=SHR
  //OUT  DD DSN=IBMUSER.LIBC370.PROBE.LINKLIB,DISP=SHR
  //SYSIN DD *
    COPY OUTDD=OUT,INDD=((IN,R))
    SELECT MEMBER=TSTXXX
  ```
- **Report via `wtof()`, not only `printf`.** SYSOUT records sit in the QSAM
  buffer until `fclose`, and any abend discards them. WTOs reach the job log
  the moment they are issued and survive anything. Pattern: `printf` for the
  human transcript, one compact `wtof("TSTXXX …: raw=values")` per case for the
  record.
- **`SYSPRINT DD SYSOUT=*` with `DCB=(RECFM=FBA,…)` eats the first character**
  of every line — byte 1 is the ANSI carriage control. Use `RECFM=FB` or no DCB.
- The effective private area is **~6M** whatever `REGION=` asks for.
- mvsmf has no job-cancel endpoint. To kill a hung probe:
  `zowe console issue command "C <jobname>"`.

---

## The X'75' red/green measurement

The pattern for proving an X'75' emulator fix, used for the restart defect and
reusable as-is.

Two commits on a `diag/` branch that differ by **one line**: red carries the
instrumentation without the fix, green carries both. Red is not "the fix
reverted" — the trace calls into the fix's helper, so reverting the file
wholesale removes the instrumentation too and measures nothing.

`x75.c` carries `X75_TRACE_RESTART`, **active** on the diag branch and absent
from the `fix/` branch. It logs every entry with `GR_L(0) != 0`:

```
X75 restart 7 (3 after a completed segment): dir=1 left=768 done=256 talk=0
```

**Read the second number, not the first.** Restarts reporting `done=0` faulted
on the first segment having copied nothing, which is correct behaviour on both
builds and proves nothing. mvsmf's byte-at-a-time `recv()` produces exactly
those (`left=1 done=0`) and is immune by construction.

The loop:

1. build red → Mike installs → verify the banner → IPL → run the probe
2. build green → Mike installs → verify the banner → IPL → run the same probe

The load module is installed **once**; DASD survives the binary change.

**The result that confirms a fix is restarts still happening, with no
corruption.** Restarts disappearing means the fault rate moved, not the resume
path, and says nothing. That is why the instrumentation stays in across both
halves.

### A guest pass, on its own, proves nothing

A clean run has two causes — the emulator is right, or the probe never faulted
— and the guest cannot tell them apart. Whatever forces the fault must be
verified from inside the probe, before the measurement, by observing its
effect rather than its return code.

The concrete instance: `PGRLSE` (SVC 112) wants R0 = low address and R1 = the
first byte **beyond** the page, because it rounds inward. With the last byte of
the page it releases nothing — and **`R15` is 0 either way**. The first red run
passed all four cases for that reason. Only filling the page, releasing it and
reading it back detects it. Build that check in, and make it refuse to run the
measurement when it fails rather than reporting a pass that cannot fail.
