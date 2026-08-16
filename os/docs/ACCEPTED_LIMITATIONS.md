# Accepted Limitations and Known Behaviors

This document is a permanent project record separating **kernel defects**,
**environmental limitations**, and **intentional design tradeoffs**. A known
limitation is not the same as broken: every entry records what works, what is
limited, and why it is acceptable today.

## Taxonomy

| State | Meaning | Entries |
|-------|---------|---------|
| WORKING | Functionally correct today; behavior verified | Bare-metal fixed IPIs, INIT/SIPI, ATA polling, SFS tests, journal metadata recovery |
| ENVIRONMENT-LIMITED | Correct on real hardware; limited only in the emulator | KVM cross-vCPU FIXED IPIs, QEMU SMP timer behavior, KVM I/O APIC |
| NOT IMPLEMENTED / NOT TESTED | Feature absent or unvalidated, by design | x2APIC mode |
| KNOWN BENIGN | Odd but harmless observable behavior | Phantom `ata2`, SFS teardown warning |
| DESIGN TRADEOFF | Deliberate consistency model choice | Filesystem durability model |
| ENVIRONMENT QUIRK — HARDENED | Emulator behavior absorbed by a permanent kernel invariant; no longer an active limitation | TCG CPU-ID behavior |

Every entry carries a classification block so the record stays useful
independently of this taxonomy.

---

## 1. KVM does not deliver cross-CPU FIXED-mode IPIs

**Category:** Virtualization limitation (environment)
**Status:** Accepted
**Scope:** QEMU/KVM only
**Affected subsystem:** SMP / TLB invalidation
**Bare metal:** Works — ICR write + delivery-status wait is the standard mechanism
**Correctness impact:** Deferred remote TLB invalidation (see invariant below)
**Workaround:** Local `invlpg` always; remote IPI only when `smp_ipi_works` is set
**Revisit when:** SMP validation moves to bare metal or KVM IPI behavior changes

**Accepted:** FIXED-mode IPIs sent to *other* vCPUs are never delivered by
KVM's in-kernel APIC; the delivery-status bit also never clears for non-self
targets. Self-IPIs and INIT/SIPI work correctly.

**Proof:**
- Session 2026-07-12 (SMP Phase 7.5): "Cross-CPU FIXED-mode IPI delivery
  **limited on KVM** (in-kernel APIC doesn't deliver to other vCPUs; INIT/SIPI
  modes work)".
- Code: `apic_send_ipi()` / `apic_send_ipi_allbutself()` skip the
  delivery-status wait (it never clears on KVM for non-self targets) —
  self-IPI and INIT/SIPI paths still wait.
- Code: `smp_ipi_works` defaults to 0; `smp_tlb_shootdown_safe()` always does
  the local `invlpg` and only sends remote IPIs when the flag is set.

**Correctness invariant — IMPORTANT:**

Per-process page tables do **not** automatically make remote stale TLB
entries safe. `sys_clone()` gives threads the same `cr3`
(`ct->cr3 = proc->cr3` in `syscall.c`), and `sched_balance_push()` freely
migrates threads between CPUs, so two threads of one address space CAN run
concurrently on two CPUs. On KVM the remote CPU never receives the
invalidation, so:

> Remote TLB invalidation is deferred. Correctness is NOT guaranteed by an
> explicit invariant; it currently holds because page-table mutation paths
> that leave stale-able translations (munmap/unmap + page reuse) are not
> exercised concurrently with same-address-space execution on a remote CPU,
> and CR3 transitions (context switch, process exit) eventually discard
> stale translations. The local `invlpg` covers the modifying CPU only.

This is therefore a **testing limitation with a potentially
correctness-affecting consequence**, not merely "the IPI is an
optimization." The code path that would close it (guaranteeing no
concurrent same-address-space execution across CPUs, or a functional remote
invalidations) is future SMP work; until then any new test that
concurrently mutates mappings in a shared address space on KVM should be
treated as potentially unsafe.

---

## 2. QEMU SMP timer quirk — APIC/PIT timer delivery stops with >1 vCPU + PCI NIC

**Category:** Emulator behavior (environment)
**Status:** Accepted, compensated
**Scope:** QEMU/KVM with >1 vCPU + PCI NIC
**Affected subsystem:** Timer / scheduler preemption / load balancing
**Bare metal:** LAPIC timer is reliable; not expected
**Correctness impact:** Stalled timer ⇒ no periodic preemption on the affected CPU
**Workaround:** RDTSC-based periodic check in `schedule()` drives `sched_balance_push()`
**Revisit when:** QEMU version changes the behavior, or timer source is revisited

**Accepted:** On QEMU (TCG and KVM), with more than one vCPU and a PCI
network device present, timer interrupts (APIC and PIC/ExtINT) can stop
being delivered after an AP comes online.

**Proof:**
- Session 2026-06-20: identified with `>1 vCPU + any PCI network device
  (e1000)` — "QEMU stops delivering timer interrupts (APIC and PIC/ExtINT)
  to the BSP after the AP comes online. Both TCG and KVM affected." A
  pre-scan of PCI config space for network devices (skip AP bring-up) was
  added as a workaround.
- Session 2026-07-04: workaround removed — SMP works with e1000 on QEMU
  10.2.2 — the quirk persisted in milder form.
- Session 2026-07-10: "APIC/PIT timer stops on KVM SMP after AP bring-up" —
  RDTSC-based ~100 ms check in `schedule()` added so `sched_balance_push()`
  still fires without timer interrupts.

## Entry 9: QEMU MDIC OP bits swapped (env quirk, hardened)

**Symptom:** PHY auto-negotiation restart never reached the PHY on QEMU
e1000e/igb; MDIC readback `0x18201140` matched the PHY READ path.

**Accepted:** QEMU v10.2.2 `hw/net/e1000x_regs.h` defines
`E1000_MDIC_OP_WRITE=0x04000000`/`OP_READ=0x08000000`, swapped vs.
Intel/Linux (`OP_READ=0x04000000`/`OP_WRITE=0x08000000`). A guest MDIC
write with Intel bits is executed as a PHY read
(`(val^data)|phy[addr]`).

**Hardening:** `e1000_phy_autoneg_restart()` selects
`op_write = hal_is_qemu() ? 0x04000000u : E1000_MDIC_OP_WRITE` via
`hal_is_qemu()` (CPUID leaf 1 ECX bit 31 + leaf 0x40000000 signatures,
`hal.c:118-137`). Real Intel silicon uses the Intel convention — verified
against Linux drivers.

## Entry 10: QEMU igb link-up requires guest ANRESTART (env quirk, hardened)

**Accepted:** In the QEMU igb model a guest soft reset (CTRL.RST) calls
`timer_del(autoneg_timer)`, so the link stays down until a guest MDIC
write of BMCR.ANRESTART (PHY addr 1) re-arms autoneg (+500 ms). Real
hardware starts autoneg natively on reset.

**Hardening:** The driver issues an unconditional MDIC ANRESTART on
e1000e/igb init (harmless on classic e1000 and on real hardware).

**Mitigation:** RDTSC-based balancing in `schedule()`; AP bring-up is no
longer skipped. QEMU-only artifact. **Scope note:** this compensates load
balancing only — it does not reproduce all semantics of a functioning
periodic interrupt. Future code that depends on timer-driven behavior other
than `sched_balance_push()` must not assume it is covered by this
workaround.

---

## 3. x2APIC mode is not currently implemented/enabled

**Category:** Unsupported feature (implementation limitation)
**Status:** Accepted
**Scope:** All platforms (kernel-wide)
**Affected subsystem:** APIC driver
**Bare metal:** xAPIC MMIO works; x2APIC hardware untested
**Correctness impact:** None — the kernel never enables x2APIC
**Workaround:** n/a (xAPIC path is the only path)
**Revisit when:** Bare-metal validation finds a machine that requires x2APIC

**Accepted:** The kernel operates exclusively in xAPIC (MMIO) mode. This is a
current implementation choice, not a QEMU limitation — it remains true on
any platform.

Distinguish precisely:
- **xAPIC MMIO path (0xFEE00000):** implemented, tested on QEMU/TCG/KVM.
- **x2APIC MSR path (IA32_APIC_BASE, MSR 0x800–0x83F):** not implemented,
  never enabled.
- **ACPI x2APIC affinity parsing (SRAT type 2):** implemented — needed for
  NUMA on bare metal where x2APIC is present even if the driver never
  switches modes.
- **x2APIC hardware validation:** not performed.

**Proof:** Boot log (all runs): `[APIC] Base MSR=0xFEE00900,
phys=0xFEE00000`; AGENTS.md invariant: "On TCG, x2APIC is disabled. All
APIC access goes through MMIO. `apic_mmio` must be mapped. `apic_write()` is
a no-op without it."

---

## 4. KVM I/O APIC is inaccessible through MMIO → legacy PIC fallback → ATA IRQs not delivered on KVM

**Category:** Emulator behavior (environment)
**Status:** Accepted, compensated
**Scope:** QEMU/KVM
**Affected subsystem:** Interrupt routing / ATA
**Bare metal:** I/O APIC accessible; interrupt-driven paths work
**Correctness impact:** Device IRQs (notably ATA) never fire under KVM
**Workaround:** ATA PIO is fully polling-based (`ata_pio_poll` + `ata_poll_drq`)
**Revisit when:** KVM MMIO behavior changes or bare-metal validation

**Accepted:** KVM's in-kernel I/O APIC is inaccessible through the MMIO
interface used by the kernel — reads return version 0. The kernel therefore
falls back to the legacy PIC; with the local APIC enabled, PIC (ExtINT)
delivery is not guaranteed, so device IRQs — notably ATA — never fire under
KVM.

**Observation vs. cause — deliberately separated:** The observable fact is
`version=0` for MMIO reads of the I/O APIC. The *hypothesized* cause is
KVM's in-kernel implementation not exposing I/O APIC state through the
MMIO/EPT path; this hypothesis has not been verified against KVM internals
and should not be asserted as fact.

**Proof:**
- Boot log (every KVM run): `[APIC] I/O APIC at 0x0xFEC00000: version=0
  (KVM in-kernel or inaccessible) — using legacy PIC`.
- Session 2026-07-17 (disk boot): ATA PIO IRQ path never fired on KVM SMP —
  replaced with polling that works with `IF=0` during early boot.

---

## 5. `gpt_scan()` probes a phantom `ata2` drive

**Category:** Known benign emulation artifact / probe behavior
**Status:** Accepted, harmless
**Scope:** QEMU (secondary ATA channel with a 0-block device)
**Affected subsystem:** ATA probe / GPT scan
**Bare metal:** Not expected (real controllers don't enumerate phantom drives)
**Correctness impact:** None
**Workaround:** None needed
**Revisit when:** n/a

**Behavior:** The ATA controller exposes a secondary channel with a 0-block
"drive" (`ata2`); `gpt_scan()` reads LBA 0 of it and logs a DRQ failure.
Harmless and pre-existing.

**Proof:**
- Boot log: `[ATA] Drive 2:  (0 sectors, 0 MB)` followed by a KDEBUG-gated
  `READ DRQ fail lba=0`; disk I/O on `ata0` unaffected.
- Session 2026-08-04: confirmed "reads work fine; harmless pre-existing
  behavior".

---

## 6. SFS self-test teardown warns `free unclaimed block`

**Category:** Known benign diagnostic warning (test path)
**Status:** Accepted, harmless
**Scope:** Boot-time SFS self-tests only
**Affected subsystem:** SFS block-owner accounting (in-memory)
**Bare metal:** Same (test path, not emulator-specific)
**Correctness impact:** None — in-memory `block_owner` accounting only; tests pass 6/6
**Workaround:** None
**Revisit when:** SFS delete-path refactor

**Behavior:** During the boot-time SFS self-tests (ramdisk and disk modes),
`sfs_free_block()` logs `[SFS] WARN: free unclaimed block N (idx M)` once
per boot — a block freed that the test path never claimed. Tests still pass.

**Proof:**
- Ramdisk boot: block 209 (idx 1); disk boot: block 1470 (idx 1291) —
  different block per filesystem state, deterministic per image.
- Session 2026-08-16: proven pre-existing via stash-build comparison — the
  unmodified kernel produces the identical warning (block 209/idx 1).

---

## 7. Filesystem durability model (metadata journaled, file data write-back cached)

**Category:** Intentional design tradeoff
**Status:** Accepted (design decision, not a defect)
**Scope:** SFS + journal
**Affected subsystem:** Filesystem consistency model
**Bare metal:** Same
**Correctness impact:** A crash can lose the most recent cached *data*
writes; committed *metadata* survives
**Workaround:** `block_sync`/`block_flush` for explicit flushes
**Revisit when:** A data journal or full durability requirement is ever wanted

**Model:**

```
metadata → journal_log → commit → checkpoint (replay + flush) → durable metadata
file data → block_write (write-back cache) → block_sync/flush → durable data
```

- **Metadata** is write-ahead journaled; `sfs_end_op()` runs
  `journal_commit()` + `journal_checkpoint()` after every operation, and
  checkpoint replays committed entries to final blocks (session 2026-08-16)
  — committed metadata is crash-safe.
- **File data** is NOT journaled; it lives in the write-back cache
  (`block.c`) and is only durable after eviction or `block_sync`.

This is an explicit, valid consistency model for a teaching OS (metadata
integrity first, data with bounded loss), not an emulator problem.

**Proof:** Code: `sfs_end_op()` → `journal_commit()` + `journal_checkpoint()`;
`sfs_write_meta()` → `journal_log()` + `block_write()`; AGENTS.md session
2026-08-16.

---

## 8. TCG `smp_cpu_id()` non-determinism — CPU identity must be cached per invocation

**Category:** Toolchain/emulator quirk that hardened the code
**Status:** Accepted (invariant in force)
**Scope:** TCG with 4 vCPUs
**Affected subsystem:** Scheduler
**Bare metal:** APIC ID read is stable; the invariant is harmless
**Correctness impact:** None with the fix in force
**Workaround:** Cache `smp_cpu_id()` once per `schedule()`; pass through call chain
**Revisit when:** Any new scheduler code touches per-CPU state

**Accepted:** On TCG with 4 vCPUs, `apic_read(APIC_REG_ID)` can be
non-deterministic across successive calls within one invocation. The
invariant that keeps the scheduler correct:

```
acquire lock → read CPU identity → use identity while lock held →
drop lock → DO NOT assume the cached identity remains valid → re-read after reacquiring
```

**Proof:**
- Session 2026-07-12: TCG 4-CPU scheduler corruption traced to `pick_next()`
  and `sched_balance_push()` calling `smp_cpu_id()` internally; fixed by
  caching the value once in `schedule()` (`pick_next(pcp)`,
  `sched_balance_push(this_cpu, pcp)`, `sched_steal_thread(this_cpu)`).
- INVARIANTS.md: "Per-CPU data accessed only via `sched_pcp()`: Never index
  `per_cpu_data[]` with a cached cpu_id across a lock drop… Re-read
  `smp_cpu_id()` after re-acquiring locks."

This document records *why* the invariant exists; `INVARIANTS.md` records
the invariant itself.

---

## Quick reference

| # | Entry | Category | Bare metal | Correctness impact |
|---|-------|----------|-----------|--------------------|
| 1 | KVM FIXED-mode IPIs not delivered | ENVIRONMENT-LIMITED | Works | Deferred remote TLB invalidation (potential) |
| 2 | QEMU SMP timer quirk | ENVIRONMENT-LIMITED | Not expected | Stalled preemption; compensated |
| 3 | x2APIC not implemented | NOT IMPLEMENTED | xAPIC works; x2APIC untested | None |
| 4 | KVM I/O APIC version=0 | ENVIRONMENT-LIMITED | Works | ATA IRQs absent; polling compensates |
| 5 | Phantom `ata2` | KNOWN BENIGN | Not expected | None |
| 6 | SFS teardown warning | KNOWN BENIGN | Same | None |
| 7 | Durability model | DESIGN TRADEOFF | Same | Bounded data loss on crash (by design) |
| 8 | TCG cpu-id non-determinism | ENVIRONMENT QUIRK — HARDENED | Stable | None with invariant |
| 9 | QEMU MDIC OP bits swapped | ENVIRONMENT QUIRK — HARDENED | Intel convention | None (`hal_is_qemu()` selects) |
| 10 | QEMU igb needs guest ANRESTART | ENVIRONMENT QUIRK — HARDENED | Not needed | None (unconditional restart, harmless) |