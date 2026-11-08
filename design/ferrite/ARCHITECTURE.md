# Ferrite — Architecture Overview

> **Status:** Draft 0.2 — design discussion, nothing implemented yet.
> **License:** MIT (everything: emulator, test suite, macros, tools).

---

## 0. Why "Ferrite"?

The name ties together the three worlds this project lives in:

- **Big Iron.** Mainframes have been called *big iron* for decades — the
  large, heavy, iron-framed machines that filled raised-floor computer rooms,
  first and foremost those from *Big Blue*. Ferrite is, literally, iron.
- **Core storage.** The main storage of the early systems this project starts
  with was built from **ferrite cores**: tiny magnetic rings threaded on wire
  grids. On many System/360 and early System/370 machines "core" *was* main
  storage, and the term survives to this day in *core image*, *core dump* and
  *low core*.
- **Rust.** Ferrite is an iron oxide — and the emulator is written in
  **Rust**. Iron that has oxidised is rust; a mainframe rebuilt in Rust is,
  in a sense, ferrite.

Old iron, the cores that held its programs, and the language used to bring
it back to life — one word for all three.

---

## 1. Purpose and Scope

Ferrite is a **greenfield mainframe emulator written in Rust**. It models a
complete **Central Processor Complex (CPC)** operated through a
**Support Element (SE)** and a **Hardware Management Console (HMC)**, in the
spirit of real machines such as the 3090 processor controller or the
Multiprise 3000 with its OS/2-based SE and EMIO.

### 1.1 Audience

- Retrocomputing and hobbyists
- Education and training (universities, self-study, courses)

Production and commercial use are **not goals**. The MIT license does not
forbid them, but the README states clearly that the project offers no support
for production workloads.

### 1.2 Goals (in priority order)

1. **Architectural fidelity**, validated against the Principles of Operation
   and, where possible, against Hercules as a reference oracle.
2. **Observability** as a first-class feature: tracing, stepping, inspection
   of PSW, registers, DAT tables, subchannels, CCW chains.
3. **Realistic operation**: power-on reset, IOCP/IOCDS, activation profiles,
   load, LPAR — learned the way it works on real hardware.
4. **Determinism and replay** for testing, debugging and teaching.
5. **Portability**: Linux and macOS first, Windows later, Raspberry Pi
   (64-bit) included.
6. **Performance**: good enough first; a JIT is an optional later phase.

### 1.3 Non-Goals (for now)

- Coupling Facility / Parallel Sysplex (must not be designed out, though).
- Running licensed IBM software in violation of its license terms.
- Bug-for-bug compatibility with Hercules.
- Full z/Architecture before the S/370 and XA families are solid.

---

## 2. Guiding Principles

| Principle | Consequence |
|---|---|
| **Clean room** | Code is written from the PoP and hardware manuals. Hercules is used as a black-box oracle only, never as a source template. Code comments cite the PoP section they implement. |
| **The architecture belongs to the partition** | A CPC hosts partitions; each partition runs in its own architecture mode (S/370, 370-XA, ESA/370, ESA/390, z). |
| **I/O frontend is per architecture, I/O backend is shared** | S/370 channels and the XA channel subsystem are different frontends over the same CCW engine and device backends. |
| **No panel in the core** | The CPC core exposes a control API only. Every operator function goes through the SE. |
| **Deterministic by default in tests** | Virtual time and deterministic CPU interleaving are core features, not add-ons. |
| **Small `unsafe` surface** | `unsafe` is confined to guest storage (`mem`) and FFI. Everything else is safe Rust. |
| **Platform-neutral core** | Core crates contain no OS-specific code; that lives in `host`. |
| **Endian-neutral** | All guest storage accesses go through explicit big-endian conversions. The code must work on little- and big-endian hosts. |

---

## 3. Architecture Lineage

```
S/370 ──(hard break: I/O, PSW, keys)──► 370-XA ─► ESA/370 ─► ESA/390 ──► z/Architecture
 own implementation                     └──── one family, facility sets ────┘   64-bit
```

| Mode | Key properties | Reference |
|---|---|---|
| S/370 | BC/EC PSW, 24-bit, channel I/O (SIO/TIO/HIO/HDV, CAW/CSW), 2K key blocks, DAT with 64K/1M segments and 2K/4K pages | GA22-7000 |
| 370-XA | XA PSW, 31-bit (BAS/BASR/BSM), channel subsystem (SSCH/TSCH, ORB/IRB/SCHIB), 4K keys | SA22-7085 |
| ESA/370 | + access registers, AR mode, extended addressing | SA22-7200 |
| ESA/390 | + additional facilities (linkage stack, etc.) | SA22-7201 |
| z/Architecture | 64-bit, new PSW/DAT formats, many facilities | SA22-7832 |

### 3.1 Modelling Strategy

- **Compile-time split for hard breaks**: `S370`, `Xa` (the XA family),
  `Z`. Each is a type implementing the `Arch` trait; generic code is
  monomorphised per architecture (replaces Hercules' triple compilation via
  `ARCH_DEP` macros).
- **Run-time facility sets within a family**: 370-XA, ESA/370 and ESA/390 are
  one implementation, gated by a `FacilitySet`. An instruction belonging to an
  inactive facility raises an operation exception, as the respective PoP
  requires.
- The **channel subsystem** is implemented once in the XA family and reused
  by z/Architecture.

---

## 4. System Model

```
┌────────────┐   REST/JSON API   ┌──────────────────────┐   Control API     ┌─────────────────────────┐
│   HMC      │ ────────────────► │  SE                  │ ────────────────► │  CPC (core)             │
│ Web UI/CLI │                   │  profiles, IOCDS,    │  in-process first │  PUs, storage, PR/SM,   │
│ n CPCs     │                   │  POR, load, logs,    │  IPC later        │  I/O frontends, SAPs,   │
└────────────┘                   │  operator console    │                   │  CCW engine, devices    │
                                 └──────────────────────┘                   └─────────────────────────┘
```

- One **HMC** manages **n CPCs**. Each CPC has exactly one SE.
- CPCs can be interconnected (CTC) and share DASD (reserve/release). Device
  backends must therefore be shareable across CPCs.

### 4.1 Model Catalog

CPCs are derived from real machine models. A catalog entry (TOML) describes
what a model *can* be configured as:

```toml
# Illustrative values — to be verified against the hardware manuals.
[model."3090-200E"]
machine_type   = "3090"
model          = "200E"
physical_pus   = 2
sap_count      = 1                     # I/O processors (not visible to the OS)
pu_types       = ["CP"]
max_storage_mb = 256
arch_modes     = ["s370", "xa", "esa370"]
max_partitions = 4                      # with PR/SM
chpid_types    = ["BLOCK", "BYTE"]
stidp_model    = 0x3090
```

The model drives what the OS sees (STIDP, later STSI/SCLP), which
configurations the SE accepts, and how many SAPs exist.

### 4.2 Processing Units

At POR the SE **characterises** the physical PUs of the model:

| Type | Emulation | Visible to OS |
|---|---|---|
| CP | CPU core of the partition's architecture | yes |
| zIIP / zAAP / IFL / ICF | **identical CPU core**, different `PuType` attribute | yes (type via SCLP/STSI) |
| SAP | runs the CCW engine (I/O worker) | no |
| Spare | unused | no |

Specialty engines are therefore *not* separate CPU implementations, only an
attribute that affects:
- what the OS learns about the CPU (so z/OS dispatches only eligible work),
- which pool the LPAR dispatcher draws from (weights and sharing per type),
- type restrictions (e.g. an IFL-only partition).

In S/370 mode there are no SAPs; the same backend is presented as "channels".

### 4.3 Partitions (PR/SM)

- Each partition (image) has: name, number, **architecture mode**, storage
  zone (origin/limit in the CPC's main storage), logical PUs with types and
  weights, and an I/O view (its subchannels or S/370 channels).
- Historically, PR/SM on the 3090 E (1988) supported partitions in different
  modes side by side. Ferrite's showcase target: **MVS 3.8j and MVS/XA in two
  LPARs on one emulated CPC**.
- Host mapping: one physical PU = one host thread (optionally pinned). The
  LPAR dispatcher maps logical PUs onto physical PUs. Phase 1: dedicated only
  (1:1). Phase 2: shared PUs with weights and capping.
- Sub-capacity models (capacity settings) can be emulated later by throttling.
- Without PR/SM the CPC runs in **basic mode**: one image owning everything.

---

## 5. Repository and Crate Layout

```
ferrite/
  crates/
    core/          # common types, errors, trace event format
    mem/           # guest storage + storage keys (the main unsafe module)
    arch/          # Arch trait, PSW abstractions, facility sets
    arch-s370/     # S/370: CPU, PSW BC/EC, DAT, interrupts, channel frontend
    arch-xa/       # 370-XA / ESA/370 / ESA/390 family, channel subsystem
    arch-z/        # later
    fp/            # HFP; BFP via SoftFloat (FFI), DFP via decNumber (FFI) — later native
    ccw/           # CCW engine, Device trait, data transfer, IDA
    devices/       # 3215, 3270, 2540, 1403, 3330/3350/3380, 3420, CTC, ...
    dasd-formats/  # CKD/FBA image formats, CCKD-compatible reader/writer
    cpc/           # CPC model: catalog, PUs, partitions, dispatcher, control API
    se/            # SE daemon: profiles, IOCP compiler, IOCDS slots, POR, load
    hmc/           # HMC: REST API, web UI, CLI
    host/          # OS abstraction: networking, mmap, threads, terminal
    oracle/        # differential test harness against Hercules
  tests/
    baremetal/     # standalone test programs per architecture (assembler)
    macros/        # shared test macro library
  catalog/         # model catalog (TOML)
  docs/
```

---

## 6. Core Abstractions (Sketches)

These are design sketches, not final APIs.

### 6.1 Architecture

```rust
/// Compile-time description of an architecture family.
pub trait Arch: Sized + Send + Sync + 'static {
    const NAME: &'static str;
    /// Storage key block size in bytes (2048 for S/370, 4096 for XA and later).
    const KEY_BLOCK: usize;
    /// Logical address type (24/31-bit in u32, 64-bit in u64).
    type Addr: Copy + Eq + Into<u64>;
    /// PSW representation of this architecture.
    type Psw: Psw;
    /// Architecture-specific I/O frontend (S/370 channels or channel subsystem).
    type Io: IoFrontend<Self>;
}

/// Run-time facility gating within an architecture family.
#[derive(Clone)]
pub struct FacilitySet(bitvec::BitArr!(for 256));
```

### 6.2 Guest Storage

Guest storage is inherently shared and mutable (n CPUs, interlocked updates,
channel DMA). To stay free of data-race undefined behaviour, **every guest
storage access is an atomic access** (`Relaxed` for normal operands, stronger
ordering for serialisation points and interlocked instructions).

```rust
/// Main storage of a CPC plus storage keys.
/// Layout is contiguous and pinnable so that it can be DMA-mapped later (FPGA).
pub struct MainStorage {
    base: core::ptr::NonNull<u8>,
    len: usize,
    keys: Box<[core::sync::atomic::AtomicU8]>, // ACC(4) F R C per key block
}

impl MainStorage {
    pub fn read_u32_be(&self, abs: u64) -> Result<u32, Addressing>;
    pub fn write_u32_be(&self, abs: u64, value: u32) -> Result<(), Addressing>;
    pub fn compare_and_swap_u32(&self, abs: u64, old: u32, new: u32) -> Result<bool, Addressing>;
    pub fn key(&self, abs: u64) -> StorageKey;
    pub fn set_reference_change(&self, abs: u64, change: bool);
}
```

A partition sees a **zone** (origin/limit) of the CPC's main storage.
Prefixing, DAT and key-controlled protection sit above this layer in the
architecture crates.

### 6.3 CPU Execution

```rust
pub struct Cpu<A: Arch> {
    psw: A::Psw,
    gpr: [u64; 16],
    cr: [u64; 16],
    pending: core::sync::atomic::AtomicU32, // interrupt request bits
    // TLB, instruction cache, timers, ...
}

pub enum StepResult { Continue, Wait, Stopped, CheckStop }

impl<A: Arch> Cpu<A> {
    /// Execute up to `budget` instructions, checking interrupts at
    /// instruction boundaries.
    pub fn run(&mut self, ctx: &mut CpuContext<A>, budget: u64) -> StepResult;
}
```

Dispatch: decoder + table/`match` interpreter first. A decoded-block cache
layer is designed in, so a JIT (Cranelift) can be added later without
restructuring the CPU.

### 6.4 I/O: Frontend, CCW Engine, Devices

```
CPU  ──SIO / SSCH──►  I/O frontend (per arch)  ──request queue──►  SAP / CCW engine  ──►  Device
CPU  ◄──interrupt───  I/O frontend             ◄──status queue───  SAP / CCW engine  ◄──  Device
```

- The **frontend** implements the architecture-visible part: S/370 channels
  (CAW/CSW, channel/device addresses) or the XA channel subsystem
  (subchannels, ORB/IRB/SCHIB, CHPIDs, path management).
- The **CCW engine** (running on SAP threads) walks channel programs:
  command and data chaining, IDA, skip, PCI, suspend/resume, key-controlled
  access to storage.
- **Devices** only execute single commands against a data transfer interface.

```rust
/// A device executes one CCW command at a time; chaining, IDA and
/// storage protection are handled by the CCW engine.
pub trait CcwDevice: Send {
    fn device_type(&self) -> DeviceType;
    fn sense_id(&self) -> Option<SenseId>;
    fn execute(&mut self, cmd: CcwCommand, xfer: &mut dyn DataTransfer) -> DeviceStatus;
    fn sense(&self) -> &[u8];
}

/// Data transfer provided by the CCW engine (guest storage behind it).
pub trait DataTransfer {
    fn count(&self) -> usize;
    fn read_from_guest(&mut self, buf: &mut [u8]) -> Result<usize, ChannelCheck>;
    fn write_to_guest(&mut self, buf: &[u8]) -> Result<usize, ChannelCheck>;
}
```

Unsolicited status (attention, device end after not-ready) goes through a
handle the device receives at attach time.

### 6.5 Control API (CPC ↔ SE)

```rust
pub trait CpcControl {
    fn power_on_reset(&mut self, iocds: &Iocds, profile: &ResetProfile) -> Result<(), CpcError>;
    fn activate(&mut self, image: &ImageProfile) -> Result<(), CpcError>;
    fn load(&mut self, image: &str, address: DeviceAddress, loadparm: &str, clear: bool) -> Result<(), CpcError>;
    fn stop(&mut self, image: &str) -> Result<(), CpcError>;
    fn start(&mut self, image: &str) -> Result<(), CpcError>;
    fn restart(&mut self, image: &str) -> Result<(), CpcError>;
    fn store_status(&mut self, image: &str) -> Result<(), CpcError>;
    fn external_interrupt(&mut self, image: &str) -> Result<(), CpcError>;
    fn display(&self, image: &str, what: DisplayRequest) -> Result<DisplayData, CpcError>;
    fn alter(&mut self, image: &str, what: AlterRequest) -> Result<(), CpcError>;
    fn step(&mut self, image: &str, cpu: u16, count: u64) -> Result<StepReport, CpcError>;
}
```

Phase 1: SE and CPC run in one process and talk through this trait.
Phase 2: the same API over IPC (e.g. gRPC or a Unix socket), mirroring the
real SE ↔ CEC service network.

---

## 7. I/O Configuration and EMIO

### 7.1 IOCP and IOCDS (XA family and later)

- Input: IOCP source in authentic syntax (`ID`, `RESOURCE`, `CHPID`,
  `CNTLUNIT`, `IODEVICE`), compiled by the SE.
- Output: IOCDS stored in slots (A0–A3). The reset profile selects the slot
  used at POR.
- Later: dynamic I/O activation via CHSC.

### 7.2 S/370 Configuration

S/370 had no IOCDS. Ferrite uses a small IOCP-like file in the same toolchain
("IOGEN-lite": channels, control units, devices with addresses) so that the
SE workflow stays the same. Exact syntax: open decision.

### 7.3 EMIO

A CHPID / channel type for **emulated I/O**: control units and devices are
mapped onto host backends.

| Backend | Used for |
|---|---|
| CKD/FBA image files (CCKD-compatible) | DASD |
| AWS / HET files | tape |
| TN3270 server (built in) | 3270 terminals and consoles |
| TCP sockets | card readers/punches, printers, CTC between CPCs |
| Userspace network stack (NAT) | network adapters, no root/driver needed |
| TAP (optional, per OS) | bridged networking |

### 7.4 Persistence

- Model catalog, activation profiles, EMIO device mapping: TOML.
- IOCP source: text. Compiled IOCDS: versioned binary/serde format.
- All SE state of a CPC lives in one directory (easy to copy and share for
  courses).

---

## 8. Time and Determinism

Two execution modes:

| Mode | Time | CPU scheduling | Use |
|---|---|---|---|
| **Deterministic** | Virtual: TOD, CPU timer and interval timer advance by instruction count (configurable rate) | All PUs of a CPC on one host thread, fixed quanta, fixed order | Oracle tests, regression tests, record/replay, teaching |
| **Real-time** | Host clock | One host thread per physical PU | Interactive use |

External events (console input, network, device completion) are injected at
defined points in deterministic mode and can be recorded and replayed.

---

## 9. Testing Strategy

### 9.1 Hercules as Oracle

- An **oracle shim** lives in the Hercules fork (QPL, a regular Hercules
  modification, *not* distributed with Ferrite). Preferably an HDL loadable
  module that adds a command and a socket endpoint.
- Protocol (binary, over TCP): `RESET`, `LOAD_CORE`, `SET_STATE`,
  `STEP n`, `GET_STATE` (PSW, GPRs, CRs, FPRs, ARs), `GET_STORAGE range`,
  `CHANGED_PAGES`.
- The harness runs Hercules and Ferrite in lockstep and compares the state
  after every instruction (or block).
- Limits: single CPU only. Clock-reading instructions (STCK, STPT, ...) are
  synchronised or masked. Hercules has **no 370-XA mode and no LPAR**:
  XA-specific behaviour and PR/SM are tested without an oracle.
  For positive XA tests Hercules in ESA/390 mode can serve as an oracle,
  because ESA/390 is effectively a superset for those programs.

### 9.2 Test Levels

1. **Instruction fuzzing**: random instructions and operands, compared
   against the oracle.
2. **Bare-metal test programs** per architecture (see 9.3). Quick to IPL,
   deterministic, identical binaries for Hercules and Ferrite.
3. **System tests**: MVS 3.8j (S/370), MVS/XA (370-XA), later ESA and z
   operating systems.
4. **Negative tests without oracle**: expected results hard-coded from the
   PoP (e.g. ESA instructions must be rejected in 370-XA mode).

### 9.3 Bare-Metal Test Conventions

- **Loading**: IPL deck/tape or core image. Both emulators can load the same
  binary.
- **Termination**: always a **disabled wait PSW** with a code in the
  address field (fits 24-bit):
  - `X'AAAAAA'` — all tests passed
  - `X'EEnnnn'` — test `nnnn` failed
  - `X'FFFFFF'` — unexpected interrupt / harness abort
- **Result block** at a fixed absolute address (default `X'3000'`, above the
  z/Architecture PSA): magic, version, test number, expected value, actual
  value, failing instruction address. The harness reads it from both
  emulators and compares.
- **Console output** (optional): 3215 CCWs for S/370 and XA, SCLP for
  ESA/390 and later.
- **Macro library** with per-architecture variants: test setup, PSW handling,
  interrupt catchers, assertions.
- **Toolchain**: HLASM-compatible cross assembler. Candidate: SATK
  (Stand-Alone Tool Kit) with its ASMA assembler; alternatives open.

### 9.4 CI

GitHub Actions matrix: Linux x86_64, Linux aarch64, macOS aarch64. Oracle
tests run on Linux (a Hercules build is part of the CI setup).

---

## 10. Platforms

| Tier | Platforms | Meaning |
|---|---|---|
| Tier 1 | Linux x86_64, Linux aarch64 (incl. Raspberry Pi 4/5, 64-bit OS), macOS aarch64 | CI on every commit, all features, oracle tests |
| Tier 2 (later) | Windows x86_64, macOS x86_64 | CI builds and tests, minor feature gaps allowed (e.g. no TAP) |
| Tier 3 | e.g. Linux s390x (big-endian) | Best effort, should work due to endian-neutral code |

32-bit hosts are not supported. Linux-only extras: FPGA via PCIe (VFIO),
SCSI tape pass-through.

---

## 11. Licensing and Dependencies

- Everything is **MIT**. Contributions use the **DCO** (`Signed-off-by`).
- Dependencies must be permissively licensed (MIT, Apache-2.0, BSD, ISC,
  Zlib, ICU/Unicode). Enforced with `cargo-deny`. No GPL/AGPL dependencies.
- SoftFloat (BSD) and decNumber (ICU) are acceptable via FFI.
- The README states the project's purpose (retrocomputing, hobby, education)
  and that production use is unsupported. The project name may be protected
  as a trademark independently of the code license.
- Clean-room rule: code is derived from the PoP and manuals, cited in
  comments. Hercules source is not used as a template.

---

## 12. Milestones

| | Goal | Done when |
|---|---|---|
| **M0** | Workspace, trace format, oracle shim in Hercules, harness skeleton | Hercules and Ferrite run in lockstep on a trivial program |
| **M1** | S/370 problem-state instructions incl. decimal and HFP | Instruction fuzzing without divergence |
| **M2** | S/370 supervisor state: PSW BC/EC, interrupts, timers, DAT, keys | Bare-metal tests green |
| **M3** | S/370 channel I/O, CCW engine, 3215/3270, DASD, tape, load | Standalone program IPLs from DASD and tape |
| **M4** | SE + control API + CLI | Load and operation only through the SE |
| **M5** | MVS 3.8j | IPL, JES2, batch jobs run |
| **M6** | 370-XA: XA PSW, 31-bit, channel subsystem, IOCP → IOCDS, POR via SE | Bare-metal XA tests green, MVS/XA IPLs |
| **M7** | PR/SM: partitions, zones, dispatcher, mixed-mode LPARs | MVS 3.8j and MVS/XA in two LPARs on one CPC |
| **M8** | ESA/370 → ESA/390 via facility sets, EMIF | Oracle tests in ESA/390 mode green |
| **M9** | z/Architecture | — |
| later | JIT (Cranelift), shared PUs, HMC web UI, multi-CPC, Windows, FPGA | — |

---

## 13. FPGA Outlook

Only two things are fixed now:

1. **Guest storage layout**: main storage and keys contiguous and pinnable,
   with a documented layout (DMA-mappable).
2. **I/O frontend ↔ CCW engine boundary as message queues** (request and
   status queues), so a SAP can be replaced by hardware.

Most promising FPGA candidates:
- **SAP / CCW engine** in hardware with DMA into guest storage.
- **Real channel interfaces** (bus & tag, ESCON) to attach real control
  units and devices.

Not pursued: a full z/Architecture CPU in an FPGA (instruction set too large,
and PCIe latency rules out frequent host traps). A S/370 soft core is
possible as a separate hobby project.

---

## 14. Open Decisions

| # | Topic | Options / Notes |
|---|---|---|
| 1 | Project name | **Decided: Ferrite** (see section 0) |
| 2 | Repository | New repo; this document moves there |
| 3 | Rust edition / MSRV | Edition 2024, MSRV policy TBD |
| 4 | SE ↔ CPC wire protocol | gRPC vs. custom over Unix socket |
| 5 | HMC UI technology | Web UI stack TBD; CLI first |
| 6 | S/370 I/O configuration syntax | "IOGEN-lite" proposal |
| 7 | DASD format | CCKD-compatible read/write vs. own format + converter |
| 8 | Device set for M3 | Proposal: 3215, 3270, 2540, 1403, 3330/3350/3380, 3420 |
| 9 | Bare-metal toolchain | SATK/ASMA vs. own toolchain |
| 10 | Oracle shim mechanism | HDL module vs. core patch; protocol details |
| 11 | Available manuals | Which PoPs, IOCP, 3090 processor controller and device manuals are at hand |
| 12 | Multi-CPC and timing | CTC/shared DASD across CPCs, TOD synchronisation (ETR/STP-like) |
