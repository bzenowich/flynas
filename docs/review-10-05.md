# arm64 port review — 2026-10-05

Review of Phases 1–5 of the DragonFly arm64 port (`docs/dfly-arm.md`) ahead of
Phase 6 (Raspberry Pi 4 bring-up).

Scope: four read-only reviews of the code, covering
- SMP and locking idiom
- memory ordering and DMA coherency
- I/O speed and durability
- a phase-by-phase gap audit.

Nothing was built or run for the review.

Code reviewed:
- fork `../dragonfly` at `d9ccd244ad` (branch arm64)
- RAID6 overlay `../hammer2-raid6` at `506ea3c`

**Status (2026-10-05, evening):** §6 items 1–3 are done, tested and
x86-checked (fork `e53e96d01f`..`1bc64454c4`, overlay `3c5c48f`..`db7d0f2`).
Item 4 waits for Phase 9. §7 gives the state of each finding; the tables
in §1–§5 are kept as found.

Paths are relative to `dragonfly/sys` unless they start with `local_`, which
means `hammer2-raid6/src/sys/`.

Marking:
- **[checked]**: re-read in the source after the review.
- **(suspected)**: inferred, not confirmed.
- No mark: reported by a reviewer from reading the code.

Why the tests did not catch these: QEMU `virt` keeps DMA coherent with the CPU
caches, models no caches at all, and TCG is more strongly ordered than a
Cortex-A72. Every cache-maintenance and memory-ordering finding below is
therefore invisible to the suites run so far (RAID6 123/0 on mmio and pci,
`-smp 2`).

## Summary

- **SMP idiom:** the foundations are DragonFly's. Where it falls short:
  - every hardware interrupt goes to cpu0;
  - one spinlock per pmap is held across range operations;
  - TLB invalidations are not batched.
- **Data loss:** not safe yet.
  - Flushes to USB disks are no-ops.
  - Two RAID6 error paths drop errors.
  - The Pi's DMA address limits are not handled at all.
- **Speed:**
  - naive copy routines;
  - synchronous, serial parity and mirror writes;
  - per-byte table parity math;
  - no UAS;
  - all interrupts on one core.
- **Phase gaps:**
  - a user-triggerable panic (`sysarch`);
  - crash dumps panic;
  - no DDB;
  - several Pi hardware services are missing.

## 1. SMP and locking

### Done the DragonFly way

None of these use FreeBSD primitives: no mtx, sx, rw, `sched_pin`, Giant or
FreeBSD `critical_enter`.

- **Interrupt deferral** mirrors x86 `ipl.s`.
  - An interrupt that arrives inside a critical section is masked at the GIC
    and EOI'd.
  - It is recorded in the per-cpu `ipending` bits.
  - It is replayed by `doreti`/`splz`.
- **IPIs:** IPIQ, cpustop and sniff each have a dedicated SGI.
  - They honour the critical section count and the `gd_npoll` handshake.
  - The per-cpu IPI queues come from `kmem_alloc3(KM_CPU)`.
  - cpusync and the IPIQ are the MI code, unchanged.
- **Timer:** per-cpu one-shot generic timer, lockless, deferred via `RQF_TIMER`.
- **TLB maintenance:** inner-shareable broadcast TLBI, no IPIs.
  - Ordering is `dsb ishst` → TLBI → `dsb ish` → `isb`.
  - Break-before-make is used when an output address changes.
  - 16-bit ASIDs with generation rollover.
- **Lock order in page-centric code:** it takes `m->spin` and only try-locks
  `pm_spin`, backing off and re-checking a generation count on failure.
- **Kernel mappings:** `pmap_kenter`/`pmap_kremove` are lockless.
- **Elsewhere:** tokens and spinlocks are used as on x86 in `trap.c`, busdma and
  the uart; serializers are wired through `nexus.c`.

### Departures

| # | Sev | Finding | Evidence | x86 |
|---|---|---|---|---|
| S1 | High | Every SPI's ITARGETSR is set to the boot cpu, and `gic_legacy_intr_cpuid()` returns 0. NIC, USB and disk ithreads all run on cpu0 and cannot be moved. There is no MSI. **[checked]** | `platform/arm64/aarch64/gic.c:394`, `:210`, `:223-250` | round-robin `gsi % ncpus` (`ioapic_abi.c:1180`), per-cpu MSI vectors |
| S2 | High | One `pm_spin` per pmap guards all its PTEs. It is held across all of `pmap_remove` (including process exit), with `vm_page_unwire`/`vm_page_dirty` called under it. All threads of a process serialize on faults, and all `kernel_pmap` enters share one lock. A cpu holding it is in a critical section, so IPIs to it are deferred and cpusync waiters stall. This was deliberate. **[checked]** | `pmap.c:49-60` (design comment), `:1856`, `:1972`, `:2030` | per page-table-page PV locks via `pmap_scan`, which can block |
| S3 | Med | TLB invalidation is per page, each with its own `dsb ish`/`isb`. `pmap_tlb_live()` adds another `dsb ish`. `pmap_remove` goes page by page up to 64 pages. `pmap_protect` (fork COW, mprotect) has no range cutoff. `pmap_qenter`/`pmap_kstore` also `dsb`/`isb` per page. | `pmap.c:265`, `:274`, `:1996`, `:2032-2054` | `pmap_inval_bulk` / `pmap_inval_smp` batching |
| S4 | Med | Switching to a kernel thread unloads the user pmap. A user → ithread → user round trip costs 2 TTBR0 writes + ISB, and 2 SEQ_CST atomics on the shared `pm_active` line, each followed by a dmb. This was a deliberate safety choice; ASIDs keep the TLB warm. | `pmap.c:1676` (comment), `:1684`; `swtch.s:271-273` | kernel threads keep the old vmspace loaded |
| S5 | Med | Global atomic counters `pmap_pv_entries` and `pmap_ptpages` are updated on every PV alloc/free, which with S8 means a full barrier on one shared line. `pv_remove` scans the page's PV list linearly under `m->spin`, so pages of shared libraries cost O(mappings). | `pmap.c:770`, `:1363`, `:1370`, `:1379` | no per-alloc global counter |
| S6 | Med | Fast paths are still stubs: `pmap_fault_page_quick` returns NULL, so `vm_fault.c:1149` always takes the slow path; `pmap_object_init_pt` is empty, so nothing is prefaulted on mmap. | `pmap.c:2404`, `:2410` | implemented |
| S7 | Low–Med | The idle loop goes straight to WFI with no adaptive spin. `cpu_mmw_pause_*` (WFE/SEV, plan §3.6) is not implemented. | `machdep.c:847-873` | spins `cpu_idle_repeat` before HLT/MWAIT |
| S8 | Low | Atomics are all SEQ_CST plus a trailing `dmb ish`, including `_acq`/`_rel` and statistics counters. This is the v1 policy (§3.7) pending a TSO audit, and it multiplies S4/S5. **[checked]** | `cpu/aarch64/include/atomic.h:54-84` | `lock`-prefixed ops |
| S9 | Low | All 32 Q registers are saved and restored eagerly on every heavy switch. This is not a bug: the kernel builds with `-mgeneral-regs-only`. | `swtch.s:157`, `:176` | lazy FPU |
| S10 | Low | `smp_stress.c` is built into every kernel; it is inert unless `debug.smp_stress` is set. `stop_cpus` has an XXX saying it is not MP-safe. (suspected) The clean-page write fault in `pmap_fault` uses a broadcast TLBI where a local one may be enough; check against the ARM ARM. | `conf/files:24`, `mp_machdep.c:581` | — |

## 2. Memory ordering and DMA coherency

### Done correctly

- **Atomics:** every read-modify-write and every cmpset/fcmpset/swap/fetchadd
  variant is an LDAXR/STLXR loop plus a trailing `dmb ish`.
  - This is stronger than x86 `lock`, including when cmpset fails.
  - `-mno-outline-atomics` is set, and no `-march` emits LSE instructions,
    which the A72 lacks.
- **Locks and refcounts:** spinlocks, `spin_access`/update seqlocks (`dmb
  ishld`/`ishst` correctly paired), tokens, `kern_mutex`, lockmgr, refcounts,
  `vm_page` busy and cpumask all release through full atomics.
  - The IPIQ was already audited (`lwkt_ipiq.c:745`, `:761`).
- **busdma:**
  - Maps are never NULL.
  - PREWRITE cleans; PREREAD cleans and invalidates (edge lines keep their
    data); POSTREAD invalidates.
  - Bounce copies happen before the clean and after the invalidate.
  - The line size comes from `CTR_EL0.DminLine`; operations are by VA, not
    set/way, and each ends in `dsb sy`.
- **`bus_dmamem`** memory is Normal-NC.
  - Both the KVA mapping and the direct-map alias are remapped.
  - Break-before-make, with a write-back-invalidate before and after.
- **Drivers:** AHCI and xHCI do a PREWRITE sync (`dsb sy`) before every doorbell
  (`ahci.c:2234-2242`, `xhci.c:3011-3034`). AHCI also wraps its registers in
  `bus_space_barrier`.
- **virtio:** the device is in the hypervisor, so inner-shareable is enough.
  - Store fence before `avail->idx`, `cpu_mfence` before notify, `lfence` after
    reading `used->idx`.
  - Fine on QEMU, but not a model to copy for real non-coherent hardware.

### Findings

| # | Sev | Finding | Evidence | Fix |
|---|---|---|---|---|
| O1 | Med | The xHCI event loop reads `dwTrb3`, tests the cycle bit, then reads `qwTrb0`/`dwTrb2`, with no barrier between. ARM may load those before `dwTrb3`, pairing a new cycle bit with a stale completion code or length. Result: a umass transfer completes with the wrong residual or status. (suspected) **[checked: no barrier]** | `bus/u4b/controller/xhci.c:1094-1106` | `dmb oshld` (Linux `dma_rmb`) after the cycle-bit check |
| O2 | Med | `bus_space_read/write_N` are plain volatile accesses with no barriers. `cpu_sfence`/`cpu_mfence`/`cpu_lfence` are `dmb ishst`/`ish`/`ishld`: inner-shareable only, and `cpu_sfence` does not order earlier loads. Drivers that skip `bus_dmamap_sync` (`busdma_machdep.c:653`) have nothing ordering descriptor stores before the doorbell. The NAS drivers (AHCI, xHCI) are safe today. **[checked fences]** | `cpu/aarch64/include/bus_dma.h:115-149`, `cpufunc.h:151-166` | Linux style: `dmb oshst` before writes and `dmb oshld` after reads in `bus_space`; or audit every Phase 6 driver (GENET, PCIe) |
| O3 | Med | The pipe reader advances `rindex` with a plain store after `uiomove()`. ARM lets that store become visible before the earlier loads complete, so the writer could refill the slot before the reader has copied it. Unlikely on an A72, but architecturally allowed. (suspected) **[checked: plain store]** | `kern/sys_pipe.c:556` | `atomic_thread_fence_rel()` before the update. More broadly, grep for `cpu_sfence()` used as a release, or map `cpu_sfence` to `dmb ish` |
| O4 | Low–Med | `lwkt_switch_return` releases the old thread with a plain `td_flags &= ~TDF_RUNNING`, and `switch_to` has no dmb after storing `TD_SP`. It is safe today only because `pmap_activate_td` happens to fence. That makes it fragile: a pmap optimisation would break it silently. | `kern/lwkt_thread.c:867`, `:870`, `:1394`; `pmap.c:1709-1718` | `dmb ish` in `switch_to` after the `TD_SP` store |
| O5 | Low | (a) A PREREAD-only sync on non-cacheable memory issues no `dsb`. (b) POSTREAD cache maintenance has no leading barrier, so `dc ivac` is not ordered after an earlier completion read. | `busdma_machdep.c:1333-1339` | `dsb` on any PRE op; a leading `dsb sy` on POSTREAD |
| O6 | Low | Edge cache lines are cleaned and invalidated at POSTREAD. A CPU store into a shared edge line during the DMA can overwrite device data. Exposed: small kmalloc buffers (CAM sense, ATA IDENTIFY, unaligned `physio`, umass, GENET RX). Buffer-cache data is page-aligned and safe. Documented as "Not yet". | `busdma_machdep.c:51-52`, `:1264-1282` | cache-line bounce (plan §3.5.3) |
| O7 | Low | `bus_space_barrier(READ\|WRITE)` issues only `dsb ld`. | `bus_dma.h:235-238` | `dsb sy` when both flags are set |
| O8 | Low | Stale comment: it says `load_acq`/`store_rel` stay full barriers, but they are LDAR/STLR, and STLR followed by a plain LDR is not ordered (unlike x86 `xchg`). Barely used in MI code (`kern_clock.c:1621`). | `atomic.h:406-408` | fix the comment, or add a dmb |
| O9 | Low | (suspected) Exclusive loads and stores on Normal-NC memory may never succeed without a global monitor. | — | make sure no driver does atomics on `bus_dmamem` memory |

## 3. I/O durability

| # | Sev | Finding | Evidence | Fix |
|---|---|---|---|---|
| R1 | **High** | `da` sets `DA_Q_NO_SYNC_CACHE` for every umass device (a stock DragonFly policy: "the port can brick"), so `BUF_CMD_FLUSH` calls `biodone()` without sending anything. HAMMER2's flush before the volume header write is therefore a no-op on the Pi, where every disk is USB. A power cut can persist a new volume header while the data, parity or space map it references is still in the drive caches, on all four members at once. FUA is not used. Invisible on QEMU: `vtblk` (`virtio_blk.c:843`) and `ahci` (`ahci_cam.c:1190`) do flush. **[checked]** | `bus/cam/scsi/scsi_da.c:1186-1188`, `:1508-1518`; `local_hammer2_flush.c:1581-1592` | **Decision needed.** (a) send SYNCHRONIZE CACHE and set the quirk only on ILLEGAL REQUEST, as FreeBSD `da` does; (b) clear WCE (mode page 8) at open; (c) a sysctl showing the state, plus a HAMMER2 mount warning or refusal when a RAID member cannot flush |
| R2 | Med | The per-disk loop overwrites `fsync_error` each pass. If an early disk fails and the last succeeds, the volume headers are written anyway. **[checked]** | `local_hammer2_flush.c:1493`, `:1507` | keep the first error, or auto-fail the member |
| R3 | Med | `(void)hammer2_io_raid6_write_row(...)` throws the result away. That function writes P and Q with `bwrite()`, so a failure never reaches `hammer2_raid6_auto_fail_disk`: stale parity on a disk still counted healthy, i.e. redundancy lost silently. The `write_nowait` calls just above are discarded the same way. **[checked]** | `local_hammer2_ondisk.c:1834`, `:1839`; `local_hammer2_raid6.c:475-484` | propagate the error and auto-fail the member |
| R4 | Med | (suspected) A disk unplugged mid-write is never marked failed. Data columns go out asynchronously (`bdwrite`/`bawrite`/`cluster_write`). A failed write re-dirties the buffer (`kern/vfs_bio.c:3556-3560`) and the flusher retries it (`:2443`). Auto-fail is only called from reads, resilver and the metadata mirror. Likely result: dirty buffers stuck in RAM, every sync erroring, possibly a hung unmount; not a panic. | `local_hammer2_io.c:854-869`; `local_hammer2_raid6.c:586`; `local_hammer2_io.c:1797`, `:2304` | fail the member on ENXIO or write EIO and invalidate its dirty buffers. The Phase 6 "survives a pulled disk" test should pull the disk *during* a write |
| R5 | **Critical (Pi)** | No DMA address limits. busdma assumes bus address == physical address. `dma-ranges` is not parsed, and nexus, simplebus and the ECAM bridge have no `get_dma_tag`, so a restricted parent tag cannot reach children. The x86 `lowaddr` bounce is inherited but never exercised (QEMU had 1–2 GB). The Pi's PCIe reaches only ~960 MB (0x3c000000), and EMMC2 only 1 GB on B0 boards. | `busdma_machdep.c:51-53`, `:312`; doc "Not yet" (dfly-arm.md ~l.1408) | Phase 6: `dma-ranges` + `get_dma_tag` + `lowaddr` bounce; bounce pool 16–32 MB (see P6) |

## 4. I/O speed

| # | Sev | Finding | Evidence | Fix / phase |
|---|---|---|---|---|
| P1 | High | `copyin`/`copyout` copy one 8-byte word per iteration with no unrolling, and fall back to one byte per iteration unless source and destination are both 8-byte aligned. `memcpy`/`memmove`/`memset` are plain C loops; `bcopy`/`bzero` go through them. Only `pagezero` uses `dc zva`. Network buffers (2-byte IP alignment) and bounce copies hit the byte path. Plan 4a assumes copies at GB/s. | `platform/arm64/aarch64/copyio.S:100-151`, `support.c:64-117`, `pmap.c:2548` | `ldp`/`stp` loops unrolled to 64 bytes with alignment fix-up (FreeBSD / Arm optimized-routines); `ldtr`/`sttr` for user copies. Cheap; early Phase 6 |
| P2 | High | Each row seal writes P, then Q, with synchronous `bwrite()`. Metadata is mirrored synchronously to every other disk, one at a time. With bulk-only umass (one command per disk) this costs several serial USB round trips per row and leaves the other disks idle. | `local_hammer2_raid6.c:475-484`, `:532+` | issue P, Q and mirror writes asynchronously (`bio_done`) and wait once. Early Phase 6 |
| P3 | High | All interrupts are on cpu0 and there is no MSI (= S1). xHCI (all four disks) and GENET share one of the four cores. | `gic.c:385`, `:394` | per-irq ITARGETSR through MachIntrABI; spread xHCI and the 2 GENET irqs; brcmstb MSI controller. Phase 6 |
| P4 | Med | No UAS: `bus/u4b/storage` has only umass, urio and ustorage_fs, so each disk handles one command at a time. 4a accepts this, but P2 makes it worse. | `bus/u4b/storage/` | later |
| P5 | Med | RAID6 parity does a 64 KB `gf_mul_table` lookup per byte, inside byte loops. The table is twice the A72's 32 KB L1D. | `local_hammer2_raid6.h:83`; `local_hammer2_raid6.c:132-138`, `:469-471` | NEON `tbl` nibble method, which needs `fpu_kern_enter` (G5); interim: 64-bit word-at-a-time multiply-by-2 (Horner). Phase 9 |
| P6 | Med | `MAXPHYS` is 128 KB, matching `UMASS_BULK_SIZE`; fine for USB, though larger I/O would cut per-command overhead. The bounce pool is `MAX_BPAGES 1024` = 4 MB; 4a wants 16–32 MB once DMA is clamped to 960 MB. | `cpu/aarch64/include/param.h:109`, `busdma_machdep.c:84` | raise for Pi boards |
| P7 | Low | No ARMv8 CRC32 instruction path: `libkern/icrc32.c` uses slicing-by-8 tables, and only metadata uses icrc32. Data checks use scalar xxhash64, which is fine. The timer is one-shot at stock `hz`, not tickless. Buffer-cache sizing copies x86 with 2 TB of KVA, which is fine for 4–8 GB. | — | optional |

## 5. Phase gaps (promised vs delivered)

### Bugs and untracked items

| Item | Phase | Status | Evidence | NAS importance |
|---|---|---|---|---|
| `sys_sysarch` is a panicking stub, reachable by any user as syscall 165 | 2 | bug **[checked]** | `platform/arm64/aarch64/stubs.c:56`; `kern/syscalls.master:237` | **High** (local DoS) |
| `cpu_set_iopl`/`cpu_clr_iopl` panic, so opening `/dev/io` as root panics | 2 | bug **[checked]** | `stubs.c:52-53`; `kern/kern_memio.c:193` | Med |
| `md_dumpsys` panics: setting `dumpdev` turns a panic into a recursive panic. No minidump, and libkvm can't read dumps; Phase 9 lists only minidump and kgdb | 2 | missing **[checked]** | `stubs.c:61` | **High** for a server |
| DDB is off, so a panic prints only a frame-pointer backtrace. Phase 9 schedules only the disassembler | 1/2 | missing, untracked | `backtrace.c` | High for Pi debugging |
| `fpu_kern_enter` (promised in Phase 2) does not exist; the kernel is `-mgeneral-regs-only` | 2 | missing | `conf/kern.mk:12` | Med (blocks NEON RAID6) |
| Kernel modules: `kmod.mk` links aarch64 modules `-Bshareable` (ET_DYN), which `link_elf_obj` rejects. `sys/modules` is not built; only a hand-linked test module was loaded. hammer2 is compiled in | 2 | partial (tracked for Phase 7) | `conf/kmod.mk:205-215` | Med |
| ptrace: debug registers and `PT_STEP` return EINVAL; no gdb/lldb is built | 2 | partial | `machdep.c:1218`; `procfs_machdep.c:98-106` | Low |
| Core dumps: register fill exists, never tested | 2/4 | untested | `kern/imgact_elf.c:1465` | Low |
| Signals: the handler's fault address comes from the last user fault even for non-fault signals | 2 | edge case | `machdep.c` `sendsig` (`tf_far`) | Low |
| ASID rollover never exercised (`vm.pmap.asid_max` exists) | 3 | untested, open | `pmap.c:612` | Med (rare SMP corruption) |
| ABI marked "PROVISIONAL" | 4 | open | `cpu/aarch64/include/ucontext.h:41`, `tls.h:45` | Med before release |
| No native compiler on the target; the clang patch was only tested at driver level. Phase 8 plan (a) depends on it | 4 | partial | dfly-arm.md ~l.1114-1127 | **High** for Phase 8 |
| PL011 is still the `early_uart.c` tty, not a `dev/serial` driver | 5 | untracked | `fdt_early.c:153` | Low |
| No minimal clk/regulator/hwreset/syscon | 5 | missing, untracked | — | High for Phase 6 |
| MSI: `gic_msi_*` return EOPNOTSUPP; GICv2m is not wired; no second-interrupt-controller layer (nexus treats any 3-cell controller as the GIC) | 5 | missing | `gic.c:223-250`; `nexus.c:436-460` | High (brcmstb MSI) |
| Phase 5 exit test ran RAID6 only on virtio, never on umass/xhci | 5 | weaker | — | Med |
| `dfly-arm.md` is stale: says 65/65; doesn't record `9e9f1a4a9a` (pmap wiring) or `d9ccd244ad` (vtblk). The 156 hammer2 "CHECK FAIL" lines in the pci run were not compared with an mmio run | 5 | untracked | dfly-arm.md ~l.22, ~l.1484, ~l.1858 | Med (records) |

### Pi hardware services

| Item | Status | Evidence | Importance |
|---|---|---|---|
| Reset/power-off: PSCI only. Reportedly the stock Pi armstub has no PSCI, in which case `reboot` hangs | gap | `machdep.c:886-890`; `psci.c:195` | **High** (Phase 6 lists the PM watchdog) |
| Watchdog: none | missing | — | High (headless NAS) |
| RTC: PL031 read only, `resettodr` does nothing; the Pi has no RTC, so it boots at 1970 until NTP | gap, not in plan | `machdep.c:1294-1322` | **High** (TLS, HAMMER2 timestamps): `ntpd -g` plus a saved-time rc script |
| Entropy: interrupt timing only; no HW RNG, and the A72 has no RNDR | gap | `kern/kern_intr.c:916` | High (first-boot sshd keys); rng200 is in Phase 6 |
| cpufreq/thermal: none. The firmware throttles on its own, but without a mailbox cpufreq driver the Pi may stay at a low clock (unverified) | optional in plan | — | Med/High for throughput |
| Spin-table, EL2 entry, early cache clean: written, untested on hardware | untested | `mp_machdep.c:169`; `locore.s:259`, `:358` | — |
| The image header needs a 2 MB-aligned load: set `kernel_address=0x200000` in `config.txt` | note | `locore.s:84-87` | Low |

### Phase 6 prerequisites not yet in the tree

- `dma-ranges` support, `get_dma_tag` passing tags from bus to child, `lowaddr`
  0x3c000000 (PCIe) / 1 GB (EMMC2 on B0), and cache-line bounce (R5, O6).
- A second-interrupt-controller layer in nexus/`gic_abi` for the brcmstb MSI
  controller and its INTx.
- brcmstb PCIe host driver (`pci_host_generic` is ECAM-only), plus the VL805
  "notify xhci reset" mailbox call.
- Mailbox and firmware-property drivers (clocks, power domains, MAC address,
  board revision); fixed-clock/clk stubs.
- EMMC2: MI sdhci has only ACPI and PCI attachments; needs an FDT/bcm2711
  attachment and the mmc bus.
- GENET: no `sys/dev/netif/genet`; the BCM54213PE PHY is not in `miidevs`.
- PM watchdog/reset, rng200, thermal, bcm2835 GPIO (MI `bus/gpio` exists), cpufreq.
- Optional: a mini-UART fallback (only `arm,pl011` is matched).
- Phase 6 step 7 (checksum-heavy I/O) is the first real test of cache
  maintenance and ordering. Run the `asid_max` rollover test and RAID6-on-umass
  there too.

## 6. Recommended order

1. ~~**Now (small, safe):**~~ **Done 2026-10-05** (`dfly-arm.md` Progress 5f).
   - `sysarch`, `cpu_set_iopl`/`cpu_clr_iopl` and `md_dumpsys` return errors
     instead of panicking.
   - R2 and R3 in the overlay. A RAID6 member whose sync or P/Q/zero-fill
     write fails is auto-failed, and a write error on the resilver target
     fails the resilver.
   - O1, O3, O4. O2 decided: `dmb oshst` before every `bus_space` write.
     Also O5 (`dsb` in `_bus_dmamap_sync` without cache maintenance), O7
     (`dsb` in `bus_space_barrier`) and O8 (`load_acq`/`store_rel` get
     their `dmb ish`; comment fixed).
   - Update `dfly-arm.md`.
   - x86 check passed (with item 3's).
2. ~~**Decision: R1 USB flush policy.**~~ **Done 2026-10-05:** (a) + (c).
   `da` sends SYNCHRONIZE CACHE to umass and sets the quirk only when the
   device rejects it (escape: `kern.cam.da.umass_sync_cache=0`).
   `kern.cam.da.N.sync_cache` shows the state, and the new `DIOCGFLUSHCAP`
   ioctl reports it. HAMMER2 warns at a read-write mount about members that
   cannot flush. Still to check on the Pi: whether the VL715 accepts the
   command. Tested with `flush.exp` (31/31). x86 check passed.
3. ~~**Early Phase 6:**~~ **Done 2026-10-05** (`dfly-arm.md` Progress 5g).
   - R5: `dma-ranges` gives simplebus and the PCIe bridge a windowed tag,
     tags carry a bus offset, and NULL-parent tags get the strictest
     window as `lowaddr`. Bouncing is tested on QEMU (tunable and a
     patched DTB); a non-zero bus offset is not, since QEMU has none.
   - S1/P3: SPIs go round-robin over the cpus (`hw.gic.irq_balance`,
     `hw.gic.irq.N.cpu`). MSI (brcmstb) is still Phase 6 proper.
   - P1: 64-byte `ldp`/`stp` loops in `mem*` and `ldtr`/`sttr` loops in
     `copyin`/`copyout`, checked over every alignment and length.
   - P2: P/Q, mirror and zero-column writes are started together and
     waited for once.
   - R4: the flush fails a member whose delayed writes returned EIO and
     invalidates its buffers. The new pull-during-write test
     (`pull.exp`) passes, but it passes on the old code too: the parity
     writes already catch the pulled disk. The new path is a backstop
     that no test reaches yet.
   - Watchdog (`bcmwd`, also the reset path without PSCI) and entropy
     (`bcmrng`, RNG200) are compile-tested only; QEMU has neither
     device. Time: the PL031 is written back by `resettodr`, the clock
     never starts before the root fs time, `rc.d/savetime` covers boards
     without an RTC, and `dntpd -s` (now the default) sets the clock
     from pool.ntp.org right after NETWORKING, before the services. It
     was tested against a fake NTP server; the real pool is untested.
   - Results on arm64 `-smp 2`, final kernel: the RAID6 suite 123/0,
     `pull.exp` 28/28, `time.exp` 23/23, `ntp.exp` 20/20. The x86 check
     also passed: `X86_64_GENERIC` with all modules builds and boots in
     h2dev, and `dntpd` builds with `-Werror`.
4. **Phase 9:**
   - P5 NEON parity (after `fpu_kern_enter`);
   - S2/S3 finer pmap locking and batched TLBI;
   - S5–S8;
   - DDB and crash dumps if not done earlier.

## 7. Status by finding (2026-10-05)

Commits are in the fork unless marked "overlay" (hammer2-raid6). The
tests are in `dfly/tools/arm-smoke`. Details are in `dfly-arm.md`
Progress 5f and 5g.

### Fixed

| # | Fix | Commit | Tested by |
|---|---|---|---|
| S1 / P3 | SPI n goes to cpu n % ncpus, and ITARGETSR follows the handler's cpu. `hw.gic.irq_balance` turns it off and `hw.gic.irq.N.cpu` pins one irq | `db037a5749` | `intr.exp` (`-smp 4`: three controllers on three cpus) |
| O1 | xhci reads the event TRB only after its cycle bit | `6ff70b9e26` | suite, storage |
| O2 | `dmb oshst` before every `bus_space` write; fences are outer-shareable | `6159fec48b` | suite, storage |
| O3, O4 | pipe `rindex` and `lwkt_switch_return` fence before the stores that publish | `d9182c584f` | suite |
| O5, O7, O8 | `dsb` in syncs that do no cache maintenance and in `bus_space_barrier`; `load_acq`/`store_rel` get `dmb ish` | `6159fec48b` | suite, storage |
| R1 | `da` sends SYNCHRONIZE CACHE to umass and sets the quirk only on rejection; `kern.cam.da.N.sync_cache`, `DIOCGFLUSHCAP`; HAMMER2 warns at mount | `a559cf9187`, overlay `6ede151` | `flush.exp` 31/31 |
| R2, R3 | The first sync error is kept; failed sync and P/Q/zero-fill writes auto-fail the member; a write error on the resilver target fails the resilver | overlay `3c5c48f` | suite |
| R4 | The flush fails a member whose delayed writes returned EIO and invalidates its buffers | overlay `db7d0f2` | `pull.exp` 28/28, but see "Fixed, not fully tested" |
| R5 | `dma-ranges` gives windowed tags (simplebus, PCIe bridge), tags carry a bus offset, NULL-parent tags get the strictest window as `lowaddr`; `MAX_BPAGES` 4096 | `ad8d0da4f7` | `bounce.exp` (tunable and patched DTB) |
| P1 | 64-byte `ldp`/`stp` `mem*`; 64-byte `ldtr`/`sttr` `copyin`/`copyout` | `7c1650325b` | `copytest.exp` |
| P2 | P/Q, mirror and zero-column writes start together and are waited for once | overlay `db7d0f2` | suite 123/0 |
| P6 (bounce pool) | 16 MB per bounce zone (`MAX_BPAGES` 4096) | `ad8d0da4f7` | `bounce.exp` |
| `sysarch`, iopl | Return `EOPNOTSUPP` instead of panicking | `e53e96d01f` | suite |
| `md_dumpsys` | Says that dumps are unsupported instead of panicking again (no dump yet) | `e53e96d01f` | — |
| Reset without PSCI | `cpu_reset_hook`, provided by `bcmwd` | `63d6773139`, `1f260d2c87` | compile only |
| RTC | PL031 started and written back (`resettodr`); never earlier than the root fs time; `rc.d/savetime`; `dntpd -s` by default, right after NETWORKING | `63d6773139`, `1bc64454c4` | `time.exp` 23/23, `ntp.exp` 20/20 |
| `dfly-arm.md` stale | Brought up to date (Progress 5f, 5g) | dfly docs | — |

### Fixed, not fully tested

- **R4:** `pull.exp` passes on the old overlay too. The parity writes,
  checked since R3, already catch the pulled disk, so the async-error scan
  is a backstop that no test reaches.
- **R5:** a non-zero bus offset can't be tested on QEMU. Check it with the
  Pi's PCIe and EMMC2.
- **R1:** whether the VL715 enclosure accepts SYNCHRONIZE CACHE is still
  to be checked on the Pi.
- **Watchdog and entropy:** `bcmwd` (PM watchdog, 15 s maximum, also the
  reset path) and `bcmrng` (RNG200 as `RAND_SRC_RNG200`) are compile-tested
  only. QEMU has neither device.
- **Time:** the real pool.ntp.org and DHCP at boot are untested; the sandbox
  has no network. The step message appears twice on the console
  (cosmetic).

### Open

| # | State | When |
|---|---|---|
| S2, S3, S5–S8 | Unchanged | Phase 9 |
| S4, S9, S10 | Unchanged, low priority | — |
| O6 | Edge cache lines are still cleaned and invalidated at POSTREAD; no cache-line bounce | Phase 6, before GENET RX and umass sense buffers |
| O9 | Unaudited | Phase 6 drivers |
| P3 (MSI) | No MSI; needs the brcmstb controller and a second-controller layer | Phase 6 step 4 |
| P4 | No UAS | later |
| P5 | Per-byte table parity; needs `fpu_kern_enter` | Phase 9 |
| P6 (`MAXPHYS`) | Still 128 KB | later |
| P7 | Unchanged | — |
| Crash dumps, DDB | No dump support, DDB off | Phase 9, or earlier if the Pi needs it |
| `fpu_kern_enter`, kernel modules, ptrace, core dumps, ASID rollover, provisional ABI, native compiler, PL011 driver, clk/regulator/syscon | Unchanged (§5) | Phases 6–9 as listed there |
| Thermal, cpufreq | Not started | Phase 6 housekeeping |
| Other Phase 6 prerequisites | brcmstb PCIe + VL805, mailbox, EMMC2 FDT attachment, GENET + PHY, GPIO, mini-UART | Phase 6 |
