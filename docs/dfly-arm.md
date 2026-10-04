# DragonFly BSD on arm64 — FlyNAS on Raspberry Pi 4 Model B

Status: **Phase 3 (SMP) is working** (2026-10-02). On QEMU `virt`, `-smp 1`,
`2` and `4` boot to a static aarch64 `/sbin/init` from an md root, and a
boot-time SMP stress test passes on 4 CPUs; the Phase 3 exit test under
load passed on 2026-10-03 (see Progress 4b). Signals, a W^X kernel image, DMAP
memory attributes and kernel module loading are done too (see Phase 2).
The console is a real PL011 tty, and **Phase 4a works**: a cross-built
static `init` drops to single-user and runs a static `/bin/sh`
interactively (see Phase 4, Progress). In 4b, libm, rtld and shared
libc/libm work (2026-10-03, dynamic programs with shared-library TLS and
dlopen pass on QEMU). **4b is done** (2026-10-03): the whole world except
`devd` cross-builds, and the Phase 3 exit test passed under load (1.5 h
of `make -j8` plus dltest loops on 4 cpus). **Phase 4 is done**
(2026-10-03): the world builds from clean on the Linux host, installs into
a UFS image, and boots on QEMU to multi-user with rc, getty, devd and
sshd. libc++ and libcxxrt (from FreeBSD) give aarch64 its C++ runtime
(see Progress 4c). **Phase 5 has started:** newbus is built from the
DTB, and the installed world boots from a virtio-mmio disk with a
virtio-mmio NIC (see Progress 5a). PCIe ECAM, busdma with cache
maintenance, AHCI and xhci + usb-storage work too (Progress 5b–5d).
**Phase 5's exit test passed** (2026-10-03): the hammer2 RAID6 suite,
forward-ported to master, runs 65/65 on 4 virtio disks (Progress 5e).
Left before Phase 6: the x86 check of the MI changes.

Goal: boot a DragonFly BSD kernel and userland on a Raspberry Pi 4 Model B
(BCM2711), with HAMMER2 (including our RAID6 patch) on USB 3 disks and
FlyNAS (OpenResty + SQLite + the Clay/WASM UI) serving over the onboard
Ethernet.

---

## 0. TL;DR

- **Nobody has done this before.** DragonFly has never had an ARM port in any
  state. The only official trace is an unclaimed "AArch64 support" code
  bounty (around 300 EUR + 200 USD, details "to be defined"). A 2012 users@
  thread estimated 1200–2000 hours for someone who already knows ARM and BSD
  kernels. Searches of GitHub forks and GSoC pages found nothing.
- **Use FreeBSD as the code donor, and Linux only as documentation.** Linux is
  GPL, so we cannot paste it into a BSD kernel. FreeBSD `sys/arm64` is
  BSD-licensed and shares DragonFly's lineage: newbus, busdma, the u4b USB
  stack, the sdhci and mii frameworks, and the same pmap ancestry. FreeBSD
  already boots the Pi 4 with every driver we need. We read Linux (and its
  device trees) only where the hardware itself is undocumented: BCM2711 PCIe,
  GENET v5, the VL805 firmware handshake, and EMMC2 DMA quirks.
- **Do bring-up on `qemu-system-aarch64 -M virt`, not the Pi.** The local QEMU
  is 8.2.2, which has no `raspi4b` machine (that arrived in 9.0). It also
  lacks PCIe and GENET even upstream. Use
  `-M virt,gic-version=2 -cpu cortex-a72`, which gives us the same CPU and the
  same GIC-400 (GICv2) programming model, plus PL011, PSCI and virtio. Only
  move to real hardware once the kernel reaches multi-user on `virt`.
- **The kernel toolchain is already on this box.** Host `clang-18` targets
  aarch64, and `tools/lld` has `ld.lld-18`. That is enough to build a
  freestanding kernel. Userland is harder: neither DragonFly's vendored GCC 8
  and GCC 12 nor upstream clang know an `aarch64-*-dragonfly` triple.
- **The hard parts, in order:**
  1. **pmap.** The DragonFly x86 pmap is 6.7K lines; it needs a new aarch64
     pmap behind the same API.
  2. **Deferred interrupts.** The `doreti`/`splz` model is x86 assembly
     (`ipl.s`) and must be rewritten.
  3. **Non-coherent busdma.** DragonFly's busdma has no cache maintenance at
     all, and the Pi 4's DMA masters are not cache-coherent.
  4. **FDT/OFW.** There is zero FDT or OFW code in the DragonFly kernel; all
     of it has to be imported.
  5. **The userland toolchain and packages.**
- **FlyNAS feature loss:** "apps as VMs" uses NVMM, which is x86 VT-x/SVM
  only. On arm64 that feature is either dropped or downgraded to QEMU TCG
  (unusably slow on a Pi). Everything else in FlyNAS is portable.

---

## 1. Why FreeBSD, not Linux, as the reference

| Concern | Linux (read for hardware behaviour) | FreeBSD (port from) | DragonFly x86_64 file being replaced |
|---|---|---|---|
| Boot entry, EL2→EL1, early MMU | `arch/arm64/kernel/head.S` (`primary_entry`, `init_kernel_el`, `__enable_mmu`), `Documentation/arch/arm64/booting.rst` | `arm64/arm64/locore.S` (1225 lines), `hyp_stub.S` | `platform/pc64/x86_64/locore.s`, `mpboot.S` |
| Machine init | `kernel/setup.c` | `arm64/arm64/machdep.c` (`initarm`) | `machdep.c` (`hammer_time`) |
| Exception vectors and traps | `kernel/entry.S` (`vectors`), `entry-common.c`, `mm/fault.c` | `exception.S` (391), `trap.c` (893) | `exception.S` (755), `trap.c` (1547) |
| Page tables, ASID, TLB | `mm/mmu.c`, `mm/context.c` (ASID rollover), `asm/tlbflush.h` | `pmap.c` (10.7K), `pte.h` | `pmap.c` (6.7K), `pmap_inval.c` (855) |
| Context switch | `entry.S` `cpu_switch_to`, `process.c` `__switch_to` | `swtch.S` (314) | `swtch.s` (895) |
| Interrupt controller | `drivers/irqchip/irq-gic.c` | `arm/arm/gic.c` (1446), `gic_fdt.c` | `apic/*`, `icu/*` (via `machintr_abi`) |
| Timer | `drivers/clocksource/arm_arch_timer.c` | `arm/arm/generic_timer.c` (953) | `apic/lapic.c` timer, `cputimer_tsc.c` |
| SMP | `smp_spin_table.c`, `psci.c`, `smp.c` | `mp_machdep.c` (960), `dev/psci/*` | `mp_machdep.c` (2016) |
| FPU/NEON | `fpsimd.c` | `vfp.c` (skip SVE) | `npx.c` (512) |
| Cache/DMA | `mm/cache.S`, `mm/dma-mapping.c` | `cpufunc_asm.S`, `busdma_bounce.c` (1210) | `busdma_machdep.c` (1469, no cache ops) |
| Atomics | `asm/atomic_ll_sc.h` | `include/atomic.h` (LL/SC + LSE) | `cpu/x86_64/include/atomic.h` (882) |
| Copyin/out | `lib/copy_*_user.S` | `copyinout.S`, `support.S` | `support.s` (877) |
| Spectre/A72 errata | `cpu_errata.c`, `proton-pack.c` | `cpu_errata.c`, `smccc*.c` | spectre/mds code in `vm_machdep.c` (drop) |
| BCM2711 PCIe | `pcie-brcmstb.c` (**only real doc**) | `bcm2838_pci.c` (784) | — |
| GENET v5 | `bcmgenet.c`, `bcmmii.c` (**only real doc**) | `arm64/broadcom/genet/if_genet.c` (1861) | — |
| EMMC2 | `sdhci-iproc.c` | `bcm2835_sdhci.c` (867) | — (DragonFly has `dev/disk/sdhci`) |
| VL805 xHCI | `reset-raspberrypi.c` | `bcm2838_xhci.c` (216) | — (DragonFly has u4b `xhci`) |

The scheduler question doesn't need Linux at all. DragonFly's schedulers
(LWKT plus `usched_dfly`) are machine-independent. The only machine-dependent
pieces are the context switch, the idle loop (`wfi`), IPIs, and the
interrupt/AST return path, all covered below.

Reference trees are sparse shallow clones in the session scratchpad (they are
not committed). Re-create them with:

```sh
git clone --depth 1 --filter=blob:none --sparse https://github.com/DragonFlyBSD/DragonFlyBSD.git dfly
git clone --depth 1 --filter=blob:none --sparse https://github.com/freebsd/freebsd-src.git fbsd
git clone --depth 1 --filter=blob:none --sparse https://github.com/torvalds/linux.git linux
# then: git sparse-checkout set <dirs from the table above>
```

**License rule for the port:** files under `sys/` must come from FreeBSD,
NetBSD, OpenBSD, or be written fresh. Linux files may be read, and register
offsets taken from them, but no code is copied. Every imported file keeps its
original copyright header plus an "Imported from FreeBSD <commit>" line.

---

## 2. Target hardware facts (BCM2711 / Pi 4B)

| Item | Value / note |
|---|---|
| CPU | 4× Cortex-A72 r0p3, **ARMv8.0-A**: no LSE atomics, no PAN, no VHE. 16-bit ASIDs. Has the CRC32 extension; NEON is mandatory. **No ARMv8 Crypto Extensions** (no AES, PMULL or SHA instructions; BCM2711 omits them). 64-byte cache lines. |
| RAM | 1, 2, 4 or 8 GB. Firmware fills in `/memory`. `/memreserve/ 0x0 0x1000` covers the armstub. The VideoCore carve-out sits at the top of the first GB. |
| Peripheral map (low-peri, default) | Legacy peripherals at `0xfe000000` (bus `0x7e000000`). ARM-local block at `0xff800000`. With `arm_peri_high=1` they move to `0x4_7e00_0000` and `0x4_c000_0000`. We use the default and read everything from the DTB. |
| Interrupts | GIC-400 (GICv2): GICD `0xff841000`, GICC `0xff842000`. Maintenance interrupt is PPI 9. The legacy ARMC/`bcm2836-l1-intc` controllers are ignored. |
| Timer | ARM generic timer, PPIs 13/14/11/10. We use the virtual timer (PPI 11 = INTID 27). CNTFRQ is 54 MHz. |
| Console | PL011 `uart0` at `0xfe201000`. Needs `dtoverlay=disable-bt` in `config.txt`, otherwise it is wired to Bluetooth. The mini-UART is the fallback and its clock moves with the core clock. |
| SD | EMMC2 at `0xfe340000` (SDHCI). B0 silicon can only DMA into the low 1 GB (30-bit); the firmware rewrites `dma-ranges` on C0. |
| PCIe | brcmstb root complex at `0xfd500000`. Outbound window: PCI `0xf8000000` → CPU `0x6_0000_0000` (64 MB). Built-in MSI controller (SPI 148), INTx on SPIs 143–146. **Inbound DMA limit:** Linux uses 3 GB. FreeBSD saw corruption above 960 MB and clamps to `0x3c000000` (a value taken from OpenBSD). Start at 960 MB. |
| USB 3 | VL805 xHCI on PCIe (`0x1106:0x3483`). Its firmware is loaded by the VideoCore through a mailbox "notify xhci reset" call (`bcm2838_xhci.c`). Newer boards load it from EEPROM, but the call is still needed after a PCIe reset. |
| Ethernet | GENET v5 at `0xfd580000` (SPIs 157 and 158), MDIO at offset `0xe14`. PHY is a BCM54213PE over RGMII. 40-bit DMA. |
| DMA coherency | **None.** Every DMA buffer handoff needs explicit `dc cvac` (clean) or `dc ivac`/`civac` (invalidate) to the point of coherency. |
| SMP release | Stock armstub: spin-table with `cpu-release-addr` 0xd8, 0xe0, 0xe8, 0xf0; secondary cores park in WFE. The firmware enters at EL2 with no PSCI and no SMCCC. With TF-A (inside the pftf UEFI build): PSCI and SMCCC, which we need for Spectre-v2 mitigation. |
| Firmware mailbox | `brcm,bcm2835-mbox` provides the property interface (clocks, power, VL805 reset, board revision, MAC address). Documented on the raspberrypi/firmware wiki. |

---

## 3. Architecture decisions

### 3.0 FreeBSD for hardware facts, DragonFly for design

FreeBSD arm64 tells us how the hardware and the ISA behave: registers, MMU and
TLB rules, GIC and timer programming, relocations, errata. It is not a template
for how the kernel is organized. Wherever FreeBSD's structure conflicts with what
makes DragonFly what it is, the arm64 code is written DragonFly's way, using
`platform/pc64` as the model:

- **SMP without contention.** Prefer per-cpu data and lockless fast paths.
  Use LWKT tokens, spinlocks and critical sections, never FreeBSD's
  mtx/sx/rw/epoch/turnstiles. Cross-cpu work goes through LWKT IPI
  messages, not `smp_rendezvous`. A global lock on a hot path is a bug, even
  when FreeBSD has one in the same place.
- **DragonFly's VM, pmap and interrupt model** (`machintr_abi`, interrupt
  threads, `pmap_inval`-style batched invalidation), not FreeBSD's pmap
  internals.
- **Keep the distinctive features possible:** HAMMER2 (busdma cache
  maintenance correct under load), swapcache, the vkernel (a `platform/vkernel`
  equivalent for arm64 later), devfs naming, dm, and the native AHCI/NVMe
  drivers (§3.5, Phase 5).
- **Userland:** vendored MI code (LLVM libunwind, compiler-rt) and ABI
  headers are used as they are. DragonFly's libc, libthread_xu and rtld stay
  DragonFly's, with only MD pieces added.

**Audit list.** Places where the current code still follows FreeBSD's
structure, to revisit:

- **ASIDs** (`pmap.c` `pmap_asid_alloc`): one global `asid_spin` and a bitmap
  scan. Lookups are lockless on the generation check, but allocation and
  rollover are serialized. Candidate: per-cpu ASID caches refilled in
  batches.
- **Context switch:** on every switch to a kernel thread, TTBR0 is loaded with
  the empty table and the `pm_active` bit is cleared. Measure this against a
  lazy approach that is still safe with respect to freed tables.

### 3.1 Names and tree layout

DragonFly splits machine-dependent code two ways:

- `sys/cpu/<MACHINE_ARCH>/` holds code specific to the instruction set.
- `sys/platform/<MACHINE_PLATFORM>/` holds code specific to the machine.

`config(8)` wires the build directory as `machine → platform/P/include`,
`machine_base → platform/P`, `cpu → cpu/ARCH/include` and
`cpu_base → cpu/ARCH`.

| Variable | Value | Why |
|---|---|---|
| `MACHINE_ARCH` | `aarch64` | Matches the ELF and compiler triple, as in FreeBSD and NetBSD |
| `MACHINE` | `arm64` | FreeBSD convention, and the name DPorts/pkg use for the ABI string |
| `MACHINE_PLATFORM` | `arm64` | One generic FDT-driven platform. The Pi 4 is a set of drivers and a kernel config, not its own platform. A later ACPI/server board reuses the same platform. |

New directories:

```
sys/cpu/aarch64/include/    atomic.h cpufunc.h cpumask.h armreg.h pte.h frame.h
                            sigframe.h ucontext.h elf.h param.h types.h vfp.h ...
sys/cpu/aarch64/misc/       elf_machdep.c lwbuf.c cputimer_generic.c db_disasm.c
                            in_cksum.c
sys/platform/arm64/
    aarch64/                locore.S exception.S swtch.S support.S copyinout.S ipl.S
                            machdep.c trap.c pmap.c pmap_inval.c vm_machdep.c
                            mp_machdep.c busdma_machdep.c vfp.c identcpu.c
                            nexus.c intr_machdep.c autoconf.c db_*.c
    include/                globaldata.h pcb.h pmap.h vmparam.h md_var.h intr_machdep.h ...
    gic/                    gic.c gic_fdt.c        (GICv2 only; GICv3 later)
    conf/                   files options kern.mk Makefile ldscript.aarch64
sys/bus/fdt/  sys/bus/ofw/  libfdt         (imported from FreeBSD)
sys/dev/soc/bcm2711/        mbox, firmware, emmc2 glue, pcie, xhci glue, gpio, rng, wdog
sys/dev/netif/genet/        if_genet.c
sys/config/RPI4  sys/config/ARM64_VIRT
```

### 3.2 Boot path

There are three stages, and each later stage keeps the earlier ones working.

1. **Stage A: QEMU `-kernel`.** The kernel image starts with a 64-byte Linux
   arm64 `Image` header, so both QEMU and the Pi firmware will load it
   directly. On entry, x0 holds the DTB physical address, the MMU is off, and
   we may be at EL2. `locore.S` drops to EL1, builds the initial TTBR1 tables
   (2 MB blocks: kernel plus DMAP of all RAM), turns the MMU on and calls
   `initarm(fdt_pa)`. There is no loader and no modules; the root filesystem
   is an md image linked into the kernel, or virtio-blk.
2. **Stage B: Pi firmware direct.** `config.txt` contains:

   ```
   arm_64bit=1
   kernel=kernel8.img
   enable_uart=1
   dtoverlay=disable-bt
   ```

   The firmware loads the image at `0x200000` (older firmware used
   `0x80000`), so the kernel must be position-independent until the MMU is
   on, as FreeBSD's `locore.S` is. Secondary CPUs are released via
   spin-table. This is the fastest route to real hardware with a serial
   console.
3. **Stage C: UEFI + `loader.efi`.** The firmware chain is either U-Boot
   `rpi_4_defconfig` (which passes the firmware DT) or pftf EDK2 in DT mode
   (which gives PSCI via TF-A). We port `stand/boot/efi/loader` to aarch64.
   The DragonFly loader already contains `R_AARCH64_*` self-relocation code
   inherited from FreeBSD. The loader passes the DTB and modules through
   `modulep`, exactly as on x86. This is required for kernel modules,
   `loader.conf` and a normal install. Start with the 3 GB RAM limit turned
   **on** in pftf, and turn it off once bounce buffers are proven.

FDT is the hardware description throughout. ACPI (pftf's default) is a
non-goal for the Pi. DragonFly's ACPICA is x86-shaped, and Pi 4 ACPI support
is a second-class path even in FreeBSD.

### 3.3 Interrupt model: keep DragonFly's, swap the backend

Don't import FreeBSD's INTRNG wholesale. DragonFly already has an abstraction
for this: `struct machintr_abi` (`sys/sys/machintr.h`), with
`intr_setup/teardown/enable/disable`, `msi_alloc/map`, `legacy_intr_*` and
`finalize`. `kern_intr.c` (`register_int`, `sched_ithd_hard`,
`ithread_fast_handler`) sits on top of it.

The plan:

- **`gic_abi.c` implements `MachIntrABI` on top of GICv2.**
  - SPIs map to DragonFly hard-interrupt numbers 0..`MAX_HARDINTS` (192).
    `MAX_HARDINTS` may need raising if GIC SPI numbers exceed it (the Pi 4
    uses SPIs up to about 180, so it is close).
  - SGIs carry IPIs, each with a dedicated SGI number: IPIQ, the
    TLB-invalidate rendezvous, stop, and the per-CPU timer if needed.
  - PPIs carry the per-CPU timer.
- **Write a small "interrupt parent" layer for FDT.** It resolves an
  `interrupts = <...>` cell triple to a GIC SPI number. This covers the
  PCIe-MSI controller, which is itself a secondary PIC. Write it fresh,
  borrowing ideas from FreeBSD's `ofw_bus_map_intr`/`fdt_intr` rather than all
  of INTRNG.
- **Port the deferred-interrupt machinery from `ipl.s` to arm64 assembly plus
  C.** It covers `doreti`, `splz`, `gd_ipending[3]`, `gd_spending`,
  RQF_INTPEND/IPIQ/TIMER/AST and `dofastunpend`. It has to live in
  `platform/arm64/aarch64/ipl.S`. The design rule: the IRQ vector acknowledges
  through GICC_IAR. If `td_critcount > 0`, it sets the ipending bit and
  RQF_INTPEND, masks that interrupt at the GIC, writes EOI and returns. Only
  later, from `crit_exit → splz`, does it run the handler and then unmask
  through `MachIntrABI.intr_enable`. This mirrors the ICU/IOAPIC
  mask-on-pending path exactly, so `kern_intr.c` needs no changes.
- Most of `doreti`/`splz` can be C called from a thin assembly shim. Only
  frame save/restore and the final `eret` must be assembly.

### 3.4 pmap: a new aarch64 pmap behind DragonFly's API

DragonFly's MI VM calls about 60 `pmap_*` functions (full list: §A.1). The
x86 pmap is PV-entry based, uses `pmap_inval.c` IPI rendezvous for every PTE
change, and has per-pmap `copyin` function pointers. The aarch64 version:

- **Address space:** 4 KB granule, 48-bit VA, 4 levels with 512 entries per
  level. Same geometry as x86_64, so `vmparam.h` constants carry over almost
  unchanged.
- **Kernel/user split:** TTBR1 holds the kernel (`0xffff...`), TTBR0 holds
  user space. User pmaps then don't contain kernel entries at all, which is
  simpler than x86.
- **DMAP:** a direct map of all physical RAM using 1 GB/2 MB blocks, as on
  x86. That keeps `PHYS_TO_DMAP` users (`vm_page2.h`, `lwbuf.c`) working and
  lets `lwbuf` stay trivial.
- **ASIDs:** 16-bit, with FreeBSD's bitmap + epoch rollover allocator
  (`pmap_alloc_asid`). Context switch writes `TTBR0_EL1` with the ASID in bits
  [63:48].
- **TLB invalidation:**
  - Use broadcast `tlbi vae1is` / `vale1is` / `aside1is` / `vmalle1is` with
    `dsb ish; isb`.
  - That makes most of `pmap_inval.c`'s IPI rendezvous unnecessary. Keep the
    `pmap_inval_*` entry points as thin wrappers so MI callers and the pmap
    stay structurally similar.
  - `smp_invltlb()` is called from `vfs_bio.c` and `kern_slaballoc.c`; it
    becomes `tlbi vmalle1is`.
- **Break-before-make:** required on ARM when changing the output address or
  block size of a live entry: invalidate the entry, `tlbi`, then write the
  new one. Implement `pmap_update_entry()` as FreeBSD does.
- **Access and dirty bits:** ARMv8.0 has no hardware A/D (that arrived in
  v8.1 as `TCR_HA/HD`). Emulate both:
  - Map pages with AF=0 and take the access-flag fault to set "referenced".
  - Map writable pages read-only with a software DBM bit and take the
    permission fault to set "modified".
  - `pmap_emulate_ad_bits` already exists in the API for vkernel/EPT, so the
    MI VM is already prepared for this.
- **Memory attributes:** `MAIR_EL1` holds device-nGnRnE, normal-NC and
  normal-WB. `pmap_page_set_memattr` and `pmap_mapdev_attr` map onto them.
  Device memory must never be accessed unaligned or by ldp/stp pairs from
  `bcopy`.
- **Approach:** start from FreeBSD `pmap.c` for page-table mechanics
  (`pmap_bootstrap`, the `pmap_l*` walkers, ASID, BBM, promotion is optional).
  Re-shape it to DragonFly's API and structures: `struct md_page`, `pv_entry`
  semantics, `pmap_object_init_pt`, `pmap_mapped_sync`, `pmap_pgscan` and
  `pmap_fault_page_quick`.
- **Skip in v1:** superpage promotion, VPTE/vkernel support (`vm_fault.c`
  VPTE paths become `#ifdef __x86_64__`), and the minidump hack.

### 3.5 busdma: add cache maintenance (this also touches MI code)

DragonFly's `bus_dmamap_sync()` macro (`sys/sys/bus_dma.h:305`) skips the
call entirely when the map is `NULL`, and `bus_dmamem_alloc` returns `NULL`
maps. On a non-coherent system that silently corrupts data. The fix:

1. Make `bus_dmamap_sync` always call into the MD layer on aarch64 (or never
   hand out `NULL` maps there). Keep x86 behaviour byte-for-byte identical.
2. **Cache operations:**
   - PREWRITE: `dc cvac` (clean) over the range.
   - PREREAD: clean any partial cache lines at the edges, then `dc ivac`.
   - POSTREAD: `dc ivac` again, to drop lines that were speculatively
     refetched while the DMA ran.
   - Round to the 64-byte cache-line size from `CTR_EL0`.
3. **Unaligned buffers:** bounce DMA buffers that are not cache-line aligned
   and are smaller than a line (FreeBSD `cacheline_bounce`). mbufs and u4b
   buffers hit this.
4. `bus_dmamem_alloc(BUS_DMA_COHERENT)` returns memory mapped normal-NC
   (uncached), so descriptor rings need no syncs. Honour `BUS_DMA_COHERENT` in
   the tag and map flags.
5. **Address limits:** bounce zones honour `lowaddr`. Set
   `lowaddr = 0x3c000000` for PCIe children (VL805, so all USB disk I/O), and
   1 GB for legacy-bus masters. Parse `dma-ranges` properly in the FDT layer,
   not with per-driver constants as FreeBSD does.

On a 4 GB or 8 GB Pi, every USB disk transfer above 960 MB bounces. That is a
real throughput cost for a NAS. Phase 9 revisits the 3 GB window.

### 3.6 Machine-independent code that leaks x86

These are cleanups to upstream DragonFly first, as they are worth doing
regardless of the port:

| Leak | Fix |
|---|---|
| `rdtsc()` and `tsc_frequency` used directly in 25 MI places (`lwkt_token.c`, `kern_spinlock.c`, `kern_clock.c`, `indefinite2.h`, `lwkt_ipiq.c`, `kern_nrandom.c`, `kern_ktr.c`) | **Revised in Phase 0:** keep the API. It is already DragonFly's generic cycle-counter interface (`_RDTSC_SUPPORTED_`). aarch64 implements `rdtsc()`/`tsc_frequency` with `CNTVCT_EL0`/`CNTFRQ_EL0`. Only `cpu_feature & CPUID_TSC` became `tsc_present`. |
| `read_rflags`/`write_rflags` in `lwkt_ipiq.c` (with `#error` for other arches) | `intr_save()` / `intr_restore()` (DAIF on arm64) |
| `bsfl`/`bsrl`/`bsfq` in MI code and `cpumask.h` | `__builtin_ctz*`/`__builtin_clz*` wrappers in `<cpu/cpufunc.h>` |
| `sys/sys/in_cksum.h` x86 `adc` asm | `#ifdef`; use the C version on aarch64 |
| `systm.h:367-383` `splz`, `setsoft*`, `cpu_mmw_pause_*` under `#if __x86_64__` | Declare MI. aarch64 implements `cpu_mmw_pause` with `wfe` + `sev`, or polls. |
| `<machine/specialreg.h>` included by `lwkt_serialize.c`, `kern_spinlock.c`, `kern_ktr.c`, `kern_clock.c`, `vm_page.c`, `vm_object.c` | Drop the include or give aarch64 a stub header |
| `kmod.mk:205` x86-only `-r` module link | aarch64 path (`-r` with lld, as FreeBSD does) |
| `vm_fault.c` VPTE and `vkernel.h` | Build only when the platform supports the vkernel |

### 3.7 Atomics and barriers

Cortex-A72 has no LSE, so atomics use LL/SC (`ldaxr`/`stlxr` loops) only.
Don't use FreeBSD's runtime `lse_supported` branch. The plan:

- Provide the full DragonFly `atomic.h` API with LL/SC, including the x86-isms
  it has grown: `atomic_cmpset_*`, `atomic_fcmpset_*`, `atomic_swap_*`,
  `atomic_fetchadd_*`, `atomic_testandset_*`, `atomic_set_cpumask`, and the
  `atomic_intr_*` bit-31 interlock used by `kern_intr.c`.
- **Map DragonFly's barriers to ARM barriers:**

  | DragonFly | aarch64 |
  |---|---|
  | `cpu_ccfence()` | compiler barrier only |
  | `cpu_lfence()` | `dmb ishld` |
  | `cpu_sfence()` | `dmb ishst` |
  | `cpu_mfence()` | `dmb ish` |
  | `cpu_pause()` | `yield` |

- **Audit for x86 TSO.** This is the subtle part. DragonFly MI code was
  written against x86's strong memory ordering (TSO). Anywhere that relies on
  "plain store then plain load is ordered", or on `atomic_*` being a full
  barrier (it is on x86 because of `lock`), may break. Rules:
  - Make every DragonFly `atomic_*` read-modify-write sequentially consistent
    (`ldaxr`/`stlxr` plus a trailing `dmb ish` where required) **in v1**.
    Only relax to `_acq`/`_rel` after testing.
  - Grep for `cpu_ccfence()` used as an ordering fence between CPUs. On x86
    that worked because of TSO; on ARM it does nothing.
  - Target the hot spots: `lwkt_ipiq.c` FIFO indices, `kern_spinlock.c`,
    `lwkt_token.c`, `kern_mutex.c`, the `vm_page` busy/wire counts, and
    `hammer2_chain` refs.

---

## 4. Phased plan

Each phase has an exit test. Effort is in focused-engineer weeks, assuming
one person who is fluent in kernels and is learning ARM as they go.
Calibrate after Phase 2.

### Phase 0 — Groundwork and tooling (≈1–2 wk)

- **Done 2026-10-01.** The fork is a full-history clone at
  `flynas/dragonfly` (sibling of `dfly/` and `hammer2-raid6/`).
  - Remote `upstream` points to github.com/DragonFlyBSD/DragonFlyBSD. Push is
    disabled (`DISABLED-no-push-to-upstream`).
  - Work happens on the local branch `arm64`, cut from `upstream/master` at
    `48147b0412` (2026-09-27) and tagged `arm64-base`.
  - No GitHub fork or `origin` remote yet. Add one when there is something
    to publish.
  - To sync: `git fetch upstream && git rebase upstream/master` on `arm64`.
  - **The base is master, not v6.4.2.** The hammer2 RAID6 overlay was
    written against v6.4.2; it was forward-ported to master on
    2026-10-03 (hammer2-raid6 `7e24841`, patch `e3f3c9a`; see Progress
    5e).
- **Done 2026-10-01: the rest of Phase 0.** There are five commits on
  `arm64`:

  | Commit | Contents |
  |---|---|
  | `6f0918352d` | config(8) builds with GNU bison and glibc |
  | `95ebf756e6` | x86 assumptions removed from MI code (§3.6) |
  | `e91212cb66` | HAMMER2 lz4/xxhash recognise aarch64 |
  | `61cab1db44` | `Makefile.inc1` accepts `TARGET_ARCH=aarch64` (TARGET/TARGET_PLATFORM `arm64`, KERNCONF `ARM64_VIRT`); `stand/` builds nothing for aarch64 yet |
  | `785d4e72f6` | arm64 platform skeleton (`platform/arm64/conf`, `config/ARM64_VIRT`) and the first ABI headers in `cpu/aarch64/include` |

  Host tooling lives in `dfly/bin` (FlyNAS repo, not the fork):

  | Tool | What it does |
  |---|---|
  | `bin/arm-hosttools` | Builds DragonFly's bmake (compiled directly from `contrib/bmake`, whose configure script is stripped), config(8) and mkdep for Linux into `tools/host/bin` |
  | `bin/arm-kbuild [KERNCONF] [targets]` | Runs config(8), then bmake, with `CCVER=llvm18`. The name must not match `clang*`, or `kern.pre.mk` adds `-no-integrated-as`. Uses host clang-18 `--target=aarch64-unknown-none-elf` and `tools/lld` ld.lld-18. Objects go in `tools/kobj/<KERNCONF>` |
  | `bin/arm-vm [-g] [-s N] [-t SECS] [KERNEL]` | QEMU `virt,gic-version=2`, Cortex-A72, `-kernel` Image boot, console teed to `logs/arm-vm.log`, timeout on by default (TCG pins one host core per vCPU). Smoke test `tools/arm-smoke/hello.S` (prints over PL011, PSCI power-off) passes in 0.25 s. `-g` needs `gdb-multiarch`, which is not installed |
  | `bin/arm-x86build {sync,build,quick,install,boottest}` | Builds the fork as x86_64 in h2dev with `-j2` (more overheats the host). `boottest` snapshots the VM, boots `/boot/kernel.arm64mi` once and goes back to stock. dloader ignores `kernel=`/`default_kernel=` in `loader.conf` (its menu is built first), so `boottest` prepends `set default_kernel=` to `dloader.rc` for one boot |

- **x86_64 verification:**
  - Unmodified master builds with h2dev's gcc 8.3.
  - With the cleanups, `X86_64_GENERIC` builds with `-Werror` and boots
    multi-user. ELF exec, pipes, 4-CPU IPIs, TCP and a HAMMER2 lz4 round
    trip all checked.
  - h2dev is back on its stock 6.4.2 kernel. The QMP snapshot
    `pre-arm64mi` is still in its qcow2.

- **Decision: keep DragonFly's TSC API instead of renaming it.** The MI code
  already treats "TSC" as the generic cycle counter: an architecture defines
  `_RDTSC_SUPPORTED_` and provides `rdtsc()`, `tsc_frequency`,
  `tsc_present`, `tsc_invariant` and `tsc_mpsync`. aarch64 implements these
  with `CNTVCT_EL0` and `CNTFRQ_EL0` in `<machine/cpufunc.h>` and
  `<machine/clock.h>`. Only the `CPUID_TSC` tests had to go. Likewise,
  `subr_cpu_topology.c`'s "APIC ID" is a generic topology key, exactly as
  vkernel64 provides it. aarch64 derives it from `MPIDR_EL1` in its
  `<machine/smp.h>`.

**Exit, met:** config(8) and bmake run on Linux, and 397 MI sources compile
up to their first missing machine header: `clock.h` (375), `atomic.h` (25),
`lock.h` (2) and `cpumask.h` (1). That list is Phase 1's starting point. A
full enumeration, rebuilding with empty placeholder headers until nothing is
missing, was started and then stopped to keep host load down. It is still
to do, at low priority (`nice`, one job).

### Phase 1 — Headers and "hello world" on QEMU (≈2–3 wk)

- Write `sys/cpu/aarch64/include/*` from FreeBSD `arm64/include`, adapted to
  DragonFly names: `atomic.h`, `cpufunc.h`, `armreg.h`, `pte.h`, `param.h`,
  `types.h`, `frame.h`, `cpumask.h` (port the x86 one using builtins),
  `elf.h`, `reloc.h`, `stdarg.h`, `limits.h`, `endian.h`, `signal.h`,
  `ucontext.h`, `sigframe.h`.
- Write `platform/arm64/include/*`: `globaldata.h` (`mdglobaldata`: ipending,
  spending, per-CPU GIC state, `gd_curpcb`), `pcb.h`, `vmparam.h`,
  `pmap.h`, `md_var.h`, `intr_machdep.h`, `clock.h`.
- Set the per-CPU pointer convention: `mycpu` comes from `tpidr_el1`. Never
  use x18; reserve it with `-ffixed-x18` just as FreeBSD does.
- Write `locore.S`:
  - Image header and EL2→EL1 drop (set `HCR_EL2.RW`, `CNTHCTL_EL2` timer
    access, `SPSR_EL2`, `eret`). Before dropping, install a minimal
    hyp-stub vector table in `VBAR_EL2` (FreeBSD `hyp_stub.S`) and record
    that we booted at EL2. This keeps EL2 reachable later through `hvc`, so
    a hypervisor (§4c) can be loaded as a module. If the kernel just drops
    and forgets, EL2 is lost until reboot.
  - Set up exception vectors early.
  - Identity map plus TTBR1 kernel mapping, then MMU on.
  - Clear BSS, set the boot stack, call `initarm`.
- Write a polled PL011 early console (`cninit` for the debugger and
  `kprintf`).
- Use a minimal FDT reader in `initarm` (libfdt imported to `sys/libfdt`) to
  find `/memory`, `/chosen/bootargs` and the stdout UART.

**Exit:** QEMU `virt` prints the DragonFly copyright banner, then panics in
`mi_startup` somewhere sensible.

- **Done 2026-10-02.** Four commits on `arm64`:

  | Commit | Contents |
  |---|---|
  | `ecf2d6a6b9` | MI: `EM_AARCH64` (was the unused `EM_res183`), aarch64 `LABELSECTOR32` |
  | `5dab66e839` | libfdt imported from FreeBSD into `sys/libfdt` (plus `fdt_memchr`) |
  | `67b8900581` | The `cpu/aarch64/include` and `platform/arm64/include` headers, enough for every MI source in ARM64_VIRT to compile |
  | `3cf8ca66bc` | Boot code: `locore.s`, `exception.S`, `hyp_stub.S`, `machdep.c` (`initarm`), `fdt_early.c`, `early_uart.c` (PL011), `support.c`, `swtch.s` (`savectx` only), `trap.c`, `stubs.c`, the Image linker script and the `kernel.bin` target |

- **What boots:**

  ```
  aarch64: entered at EL1, kernel at pa 0x40200000, DTB at pa 0x48000000
  aarch64: linux,dummy-virt
  aarch64: console uart at pa 0x9000000
  aarch64: 1024 MB memory at pa 0x40000000
  Copyright (c) 2003-2026 The DragonFly Project.
  ...
  panic: pmap_map: not yet
  ```

  It also boots when entered at EL2 (`ARM_VM_MACHINE=virt,gic-version=2,virtualization=on`):
  the hyp stub is installed and the kernel drops to EL1. A test `brk` from `initarm`
  went through the vectors and printed ESR, ELR and all registers.

- **Design as built:**
  - **Boot page tables.** Six pages in a `.boot_pt` section after BSS, so
    clearing BSS doesn't wipe them.
    - TTBR0: 1GB identity blocks for the gigabytes holding the kernel and the DTB.
    - TTBR1: KERNBASE mapped to the load address in 2MB blocks, plus 8 blocks of slack.
    - The L1 slot after KERNBASE's (0xffffffffc0000000) holds an
      initially empty L2 table. `early_devmap()` fills it with 2MB
      Device-nGnRE blocks; the UART is mapped there.
  - **MAIR.** Index 0 is Device-nGnRnE, 1 is Normal NC, 2 is Normal WB, 3 is Normal WT and 4 is
    Device-nGnRE. The x86 `PAT_*` names map onto these in `<machine/pat.h>`.
  - **`initarm()`.** Mirrors `hammer_time()`:
    - Sets up globaldata, with TPIDR_EL1 pointing at it.
    - Parses the FDT: `/chosen/bootargs` gives `-s`/`-v`/... as `boothowto` and
      `name=value` pairs as `kern_envp`. The UART comes from `stdout-path`,
      with bus `ranges` translation for the Pi.
    - `init_param1`, `mi_gdinit`, `cpu_gdinit`, `mi_proc0init`, `init_locks`, `cninit`.
    - Sets `physmem` from `/memory`, then `init_param2`, then the message buffer, which is
      static in the image for now.
  - **Stubs.** `stubs.c` holds 131 MD symbols, each of which panics with its name. They are grouped by the
    file that will replace them (pmap, user copy, bus_dma, switch,
    signals/ptrace, SMP, interrupts, RTC, kernel linker, dumps).
  - **DDB is off in ARM64_VIRT** until `db_interface.c`, `db_trace.c` and `setjmp`
    exist. `machdep.c` has a non-DDB `Debugger()`, because HAMMER2 calls it
    unconditionally.
  - **After a panic.** `boot()` spins in its `for(;;)` because the shutdown handlers
    register in a sysinit after VM init. The VM sits there until the `arm-vm` timeout.
    `cpu_reset()` halts; PSCI reset comes later.
- **Tooling changes:**
  - The `arm-kbuild` cc wrapper passes `-Wl,-shared` when it sees
    `-shared`, because the bare-metal clang driver drops it and `hack.So` came out as an
    executable.
  - `genassym.sh` works unchanged with llvm-nm.
  - `arm-kbuild` always wipes the objdir. For incremental work, run bmake in
    `tools/kobj/ARM64_VIRT` with the same variables, and rerun config(8) into
    that directory without the `rm`.
- **Not done:** an x86_64 rebuild in h2dev for the two MI header changes.
  Both are preprocessor-only and inert on x86: the disklabel32 condition is
  unchanged for x86, and `EM_res183` had no users. Run it before upstreaming.

### Phase 2 — pmap, exceptions, timer, interrupts: single CPU to `init` (≈8–12 wk)

This is the heart of the port.

- **Exceptions:**
  - `exception.S`: the 16-entry vector table and a full `trapframe`
    (x0–x30, sp_el0, elr, spsr, esr, far).
  - `trap.c`: decode `ESR_EL1` classes. Data/instruction aborts go to
    `vm_fault` via `trap_pfault`. Alignment faults panic or SIGBUS. `svc` goes
    to `syscall2`. BRK goes to the debugger.
  - Wire `pcb_onfault` recovery.
- **pmap v1 (§3.4).**
  - Bootstrap the DMAP and kernel map.
  - `pmap_kenter`/`qenter`, `pmap_enter`/`remove`/`protect`, `pmap_extract`,
    `pmap_zero_page`/`copy_page` using `dc zva`, page-table pages from
    vm_page, PV lists, and A/D emulation.
  - Run single-CPU first, using local `tlbi` only.
- **GICv2 + `gic_abi.c` + `ipl.S` (§3.3):** the IRQ vector, the critical
  section deferral path, `splz` and `doreti` with the AST check.
- **Generic timer:**
  - `cputimer` reads `CNTVCT_EL0` at `CNTFRQ`.
  - `cputimer_intr` is a one-shot via `CNTV_TVAL_EL0`, PPI 27.
  - Register it in `kern_cputimer.c` and set `cpu_cyclecount` (§3.6).
- **Context switch:**
  - `swtch.S`: `cpu_lwkt_switch`, `cpu_heavy_switch`,
    `cpu_lwkt_restore`/`cpu_heavy_restore`/`cpu_kthread_restore`,
    `cpu_exit_switch`, `cpu_idle_restore`, `savectx`.
  - Keep DragonFly's convention of the "restore function pointer at `td_sp`"
    (`cpu_set_thread_handler`, `cpu_fork` in `vm_machdep.c`). Store it in
    the pcb instead of pushing it, since there's no `ret`-to-stack idiom on
    ARM. Callee-saved registers are x19–x29, sp and lr.
  - Heavy switch also writes `TTBR0_EL1` with the ASID and `tpidr_el0` (user
    TLS).
- **FPU:** start with eager save/restore of q0–q31, fpcr and fpsr for user
  threads (simplest, ~512 bytes). Use `CPACR_EL1.FPEN` trapping for lazy
  switching later. Kernel uses `-mgeneral-regs-only`.
  `fpu_kern_enter`/`kernel_fpu_begin` exist for crypto and RAID6.
- **Root filesystem:** an md root (a UFS or HAMMER2 image built by makefs or
  newfs on h2dev, embedded via `MD_ROOT`), or virtio-mmio blk (DragonFly has
  virtio-pci; add the mmio transport or use QEMU `virt`'s PCIe ECAM with
  `pci_host_generic`).

**Exit:** single CPU on QEMU `virt` reaches `init` from a statically linked
aarch64 `init` and `sh` (built in Phase 4a), or at least executes a hand-made
static ELF that does a `write` syscall and `exit`.

**Progress (2026-10-02): exit test passed.**

- **Commits** on `arm64`:

  | Commit | What |
  |---|---|
  | `06cb419c85` | pmap v1: DMAP of all RAM, kernel page tables grown on demand, user pmaps in TTBR0 with ASIDs, per-page PV lists, software A/D emulation via `pmap_fault()`, broadcast TLBI; `phys_avail[]` from `/memory`, `/memreserve/`, `/reserved-memory` |
  | `77adc5f880` | GICv2 (`MachIntrABI`), generic timer (cputimer, cpucounter, cputimer_intr on PPI 27), IRQ dispatch with x86-style pending/`doreti`/`splz`, trivial topology |
  | `35a000a2a3` | gtimer fix: `gtimer_intr_enable()` disarmed a timer that `gd_timer_running` still claimed was armed, so no tick ever fired |
  | `3bb59b6fa2` | Context switch, `vm_machdep.c`, `trap.c`, `copyio.S`, `cpu_startup`, `cpu_idle`, `exec_setregs`, TLS, ELF brand, PL031 `inittodr`, initrd as md root, minimal `/dev/console` |

- **What runs:**

  ```
  $ clang-18 --target=aarch64-unknown-dragonfly -c tools/arm-smoke/uinit.S -o uinit.o
  $ mkdir -p root/sbin root/dev
  $ tools/lld/usr/bin/ld.lld-18 -static -e _start uinit.o -o root/sbin/init
  $ bin/arm-mkiso root.iso root
  $ bin/arm-vm -r root.iso -a vfs.root.mountfrom=cd9660:md0
  ...
  Mounting root from cd9660:md0
  Mounting devfs
  uinit: hello from aarch64 userland
  init died (signal 0, exit 0)
  panic: Going nowhere without my init!
  ```

- **Design as built:**
  - **Switch protocol.** `td_sp` points at a 16-byte slot `{restore
    function, DAIF}`. The switch stores the old sp in `td_sp`, sets
    `gd_curthread`, loads the new `td_sp` and branches to the slot's
    function with x0 = new and x1 = old. Every restore calls
    `pmap_activate_td`. Heavy switches save x19–x30/sp in the pcb, and
    eagerly save q0–q31/FPSR/FPCR and `tpidr_el0`. Kernel threads start in
    `cpu_kthread_restore` (function, argument and return address in
    `pcb_x[0..2]`), forked user threads in `fork_trampoline`.
  - **Syscall ABI** (FreeBSD-like, provisional until libc): `svc #0`, number
    in x8, arguments x0–x7 then the user stack, results in x0/x1, errors as
    errno in x0 with PSTATE.C set, ERESTART backs `elr` up by 4.
    `exec_setregs` passes x0 = the stack (argc), x1 = `ps_strings`.
  - **User access.** `copyio.S` uses LDTR/STTR after a range check against
    `VM_MAX_USER_ADDRESS`. `pcb_onfault` recovery happens in `trap_pfault`.
    The user atomics (`casu*`, `swapu*`, `fuwordadd*`) use LDAXR/STLXR and
    rely on PAN being off.
  - **Root.** QEMU `-initrd` lands in `/chosen linux,initrd-*`. `fdt_early`
    reserves it, and `initarm` publishes it as an `md_image` preload record,
    the record `md(4)` already looks for. There are no image tools on the Linux host, so
    `bin/arm-mkiso` writes a plain ISO 9660 (`options CD9660`) from a
    directory.
  - **Console.** `/dev/console` forwards to `ttyu0`, the PL011 in
    `early_uart.c`. It started as a write-only polled device; since fork
    `395fd3eff2` it is an interrupt-driven tty (see Phase 4, Progress).
    `inittodr` reads the PL031 when the DTB has one.
- **Hazards closed after the exit test (2026-10-02):**
  - **Signals** (fork `f3e8ad4ef0`).
    - `sigtramp.s` holds the sigcode: `blr x8` to the handler, then
      `sigreturn(&sf_uc)`, falling back to `exit`.
    - `sendsig` puts the handler's arguments in x0–x3: signo, siginfo or
      code, ucontext, fault address.
    - The FP/SIMD state is part of the mcontext (`_MC_FPFMT_VFP`).
    - `sys_sigreturn` takes only the NZCV flags from the user's SPSR, and
      refuses anything that is not EL0t AArch64 with the current DAIF.
    - ptrace and procfs `fill/set_regs` and `fpregs` are implemented.
      Debug registers and `PT_STEP` return EINVAL.
    - `tools/arm-smoke/sigtest.c`, run as `/sbin/init`, checks:
      - registers and FP state survive a handler;
      - a SIGSEGV is resumed through an edited context;
      - a forged EL1 SPSR is refused.
  - **W^X kernel image** (`051061883d`).
    - The ldscript puts `_kdata` on the 2MB boundary after the RO segment.
      Blocks below it are RO, and only they are executable.
    - The sysinit sets moved to data, because `mi_startup` sorts them in
      place.
    - The 2MB tail of the last block is not given to the VM, so no free
      page is aliased through KERNBASE.
    - A kernel fault on the image or the DMAP is now fatal; before, it went
      to `vm_fault`.
    - The DMAP still has a writable alias of the text, as on FreeBSD.
  - **DMAP memory attributes.**
    - `pmap_page_set_memattr` and `pmap_change_attr` now change the DMAP
      too. They split 1GB/2MB blocks with break-before-make, and
      `pmap_dmap_fault()` retries the faults that other CPUs take during
      the gap.
    - `vm_page_alloc_contig` (MI) now resets the attribute of reused pages.
    - `smp_stress` flips a DMAP range UC↔WB while the other CPUs read it.
  - **Kernel modules** (`6bfd5cc205`).
    - `elf_reloc` handles the static relocations of ET_REL modules, for
      both the small and the large code model, with range checks.
    - `link_elf_obj` syncs the icache after relocating.
    - **Found along the way:** the arm64 `pmap_enter` did not do the
      `vm_page_wire()` that x86's does and `vm_fault` depends on. So
      kernel-wired memory was not wired at all, and `pmap_unwire` did not
      advance `*pva`. Both are fixed.
    - `tools/arm-smoke/kmodtest/` is a module that checks every relocation
      type, plus an init that loads and unloads it.
  - `pmap_object_init_pt` stays a no-op. It is only a prefault
    optimization, not a hazard.
- **x86_64 check** (2026-10-02): `bin/arm-x86build build` of fork
  `6bfd5cc205` (all MI changes up to here) builds `X86_64_GENERIC` in h2dev,
  and `boottest` boots it to multi-user.
- **Debugging aids:**
  - `bin/arm-vm -a "-v ..."` boots verbose.
  - With no gdb on the host, start QEMU with `-monitor unix:...` and poll
    `info registers` for PC/X30. Resolve with
    `llvm-addr2line-18 -fi -e tools/kobj/ARM64_VIRT/kernel.debug`.
  - A CPU sitting in `cpu_idle`'s WFI means every thread is blocked. Dump
    `gd_tdallq` (`td_comm`, `td_wmesg`) from the idle loop to see why.

### Phase 3 — SMP (≈3–4 wk)

- Bring up APs in `mp_machdep.c`:
  - PSCI `CPU_ON` (HVC on QEMU, SMC under TF-A).
  - Spin-table: write the entry point to `cpu-release-addr`, `dc civac`,
    `sev`. Note that FreeBSD assumes one shared release address; the Pi has
    one per CPU.
  - Each AP runs `init_secondary`: per-CPU `mdglobaldata`, the GIC CPU
    interface, the timer PPI, and its idle thread.
- IPIs via GICD_SGIR: wire `cpu_send_ipiq`, `smp_invltlb` (now just
  broadcast TLBI), `stop_cpus`/`restart_cpus` and `cpu_sniff`.
- Re-validate `pmap_inval` with broadcast TLBI under SMP. Run the TSO audit
  (§3.7) for real: stress `lwkt_ipiq`, tokens, spinlocks, the slab allocator,
  and `buildworld` of something large.
- Port `detect_cpu_topology` / `subr_cpu_topology.c` from `MPIDR_EL1`
  affinity fields (one package with 4 cores).

**Exit:** `-smp 4` boots on QEMU and survives a few hours of parallel
`make -j8` plus `stress`-like workloads with `INVARIANTS`.

#### Progress (2026-10-02)

- **Commit:** fork `a021b024bb` "arm64: SMP: PSCI, AP start-up, GIC SGI
  IPIs, SMP-safe ASIDs".
- **What runs:** `bin/arm-vm -s 4 -r root.iso -a "vfs.root.mountfrom=cd9660:md0 debug.smp_stress=20"`
  prints `SMP: 4 cpus started` and the topology (4 cores, 1 chip). It then
  runs 20 s of `smp_stress` (454k iterations, 0 errors) and the static
  init. The resulting panic stops the other CPUs and resets through PSCI.
  `-smp 1` and `-smp 2` pass too. `INVARIANTS` is on.
- **Design as built:**
  - **PSCI** (`psci.c`): the conduit comes from `/psci` `method`. It is HVC on
    QEMU without EL2, and SMC under TF-A or QEMU `virtualization=on`; the
    kernel's own hyp stub never sees PSCI calls. PSCI 0.1 takes its function
    ids from the DT. `cpu_reset` → SYSTEM_RESET; `RB_POWEROFF` → SYSTEM_OFF
    via a `shutdown_final` handler.
  - **CPU enumeration:** `mp_probe()` runs from `initarm`, because
    `subr_cpu_topology.c` needs `naps` before the APs start. It reads the
    `/cpus/cpu@*` `reg` (MPIDR), `status` and `enable-method`. For psci it
    uses the PSCI firmware; for spin-table it uses that CPU's own
    `cpu-release-addr`.
  - **Topology:** `get_cpuid_from_apicid` scans `naps + 1` entries, not
    `ncpus`, for the same reason. The packed hwid is Aff2:Aff1:Aff0: chip =
    hwid >> 8 (cluster), core = Aff0.
  - **AP start** (`start_all_aps`, at `SI_BOOT2_START_APS`) mirrors x86:
    - Each AP gets a `kmem_alloc3` privatespace, `mi_gdinit`/`cpu_gdinit`,
      an ipiq array and arc4.
    - The APs start one at a time. `bootAP`/`ap_boot_sp` are globals, so no
      register context is needed (spin tables pass none). The BSP waits up
      to 5 s for the AP's bit in `smp_startup_mask` and panics otherwise.
    - `locore.s` `mpentry` runs `enter_kernel_el` and turns the MMU on with
      the boot tables. `boot_l0_id` still identity-maps the kernel GB, and
      `boot_l0_k` is the kernel pmap's L0. TCR/MAIR come from the BSP via
      `ap_boot_tcr`/`ap_boot_mair`, cleaned to PoC because the AP reads them
      with the MMU off. It then jumps to KVA, sets `vbar_el1` and calls
      `init_secondary()` (TPIDR_EL1, CPACR, empty TTBR0). Finally it
      "switches" into the idle thread's `cpu_idle_restore` slot, which calls
      `ap_init()` on an AP.
    - `ap_init`/`ap_finish` are the x86 interlocks, with `gic_init_cpu()` in
      place of the LAPIC and `smp_gic_mask` in place of `smp_lapic_mask`.
  - **IPIs** are GIC SGIs: 0 IPIQ, 1 CPUSTOP, 2 SNIFF. `gic_ipi()` does a
    `dsb ishst` before the GICD_SGIR write.
    - `do_irq` → `sgi_intr()`. IPIQ follows x86 Xipiq: `gd_npoll` is swapped
      to 0 before processing, or RQF_IPIQ is set in a critical section;
      `doreti`/`splz` now swap `gd_npoll` too.
    - CPUSTOP follows Xcpustop (`cpustop_handler()` in `mp_machdep.c`).
      Stopped CPUs wait in WFE (`cpu_smp_stopped`), and `restart_cpus`
      sends SEV.
    - GICv2 addresses at most 8 CPUs.
  - **ASIDs** follow FreeBSD's scheme:
    - A per-generation bitmap. A rollover carries over the ASIDs of the
      previous generation that some CPU's `gd_curpmap` holds, and flushes
      (`tlbi vmalle1is`) only after that scan, under `asid_spin`.
    - `pmap_activate_td` sets `gd_curpmap` before loading a pmap. Before a
      different one, it first loads the empty table and only then clears
      `gd_curpmap`, so that a rollover never reuses an ASID a CPU may still
      walk with.
    - `pmap_tlb_live()` does `dsb ish` before the generation check (pairs
      with the allocator's store and TTBR0 load).
    - `pmap_release` frees the ASID.
    - Loader tunable `vm.pmap.asid_max=N` shrinks the ASID space to force
      rollovers. It is untested until there is more than one user process.
  - **`smp_stress.c`**: loader tunable `debug.smp_stress=SECONDS` runs one
    thread per CPU before root mount. Each thread exercises atomics, a
    spinlock and a token around plain counters, `lwkt_send_ipiq` to the next
    CPU, `lwkt_cpusync_simple` to all, and cross-CPU kmalloc/kfree of
    pattern-filled blocks. It also runs a kernel TLB shootdown check: cpu 0
    `pmap_kenter`s one VA over 4 pages under an exclusive spinlock, and the
    others read it under a shared one. All counts are checked at the end.
- **Still open:**
  - ~~The literal exit test (hours of `make -j8`).~~ Passed 2026-10-03
    on the cross-built world (see Progress 4b).
  - The ASID rollover path is not exercised yet (one user process).
  - The spin-table path is untested until the Pi.
  - All SPIs still go to cpu 0.
  - No `cpu_send_ipiq_passive`.
  - `detect_cpu_topology` is empty: the hwid functions do the work.

### Phase 4 — Userland (≈6–10 wk, can overlap Phases 2–3)

**4a. Toolchain.** Choose LLVM over GCC.

- **Why not GCC:** DragonFly's vendored GCC 8 and GCC 12 have only
  `config/i386`, and adding `aarch64-*-dragonfly*` to GCC means touching
  `config.gcc`, libgcc and the vendored build glue.
- **Clang changes:** teach it `aarch64-unknown-dragonfly`. That needs
  `Targets.cpp` (OS switch → `DragonFlyBSDTargetInfo<AArch64leTargetInfo>`)
  and `Driver/ToolChains/DragonFly.cpp` (`crt` paths, dynamic linker
  `/usr/libexec/ld-elf.so.2`, `-m aarch64elf`). It's a small patch, about
  50 lines, modelled on the FreeBSD aarch64 case.
- **Build:** a cross clang + lld on Linux. Build world with `CCVER=custom`
  pointing at it.
- **Native toolchain:** build the same LLVM natively for the target (it
  comes later as a package, `devel/llvm`).

**4b. Libraries and runtime.**

- `lib/csu/aarch64` and `libexec/rtld-elf/aarch64`: `reloc.c` with TLSDESC
  and `rtld_start.S`, from FreeBSD.
- `lib/libc/aarch64`: `gen/` (setjmp, `_ctx_start`, makecontext, fp masks),
  `string/` (start with the C versions), and DragonFly's syscall stub
  generator (`SYS.h` uses `svc #0`, x8 = syscall number, carry flag means
  error, as on FreeBSD).
- `libthread_xu` MD bits (`thr_machdep`, TLS via `tpidr_el0`). TLS variant I
  on aarch64, unlike variant II on x86: this touches `sys/tls.h`, rtld's TLS
  allocator and `libc/gen/tls.c`.
- `libm` aarch64 (FreeBSD `msun/aarch64`), `libkvm`, `libstand` and anything
  `MACHINE_ARCH`-gated in `lib/`, `bin/`, `sbin/`, `usr.bin/`, `usr.sbin/`.
- **Kernel side** (done, see Phase 2):
  - `sendsig`/`sigreturn` with a `sigframe` containing `ucontext` and `fpregs`.
  - `exec_setregs`, ptrace `fill/set_regs`, `sigtramp` in `sigcode`.
  - `cpu_sanitize_frame` checks SPSR: EL0 only, DAIF cleared.
- **ABI:** decide `MACHINE_ARCH=aarch64` ABI constants, ELF OSABI and branding
  (the DragonFly note `.note.tag`) before anything ships. Get these reviewed
  against upstream DragonFly so we don't fork the ABI.

**Exit:** `make buildworld TARGET_ARCH=aarch64` on the Linux host produces
a world. `installworld` to a disk image, boot it on QEMU `virt` to multi-user
with `sshd`.

#### Progress (2026-10-02)

- **PL011 tty** (fork `395fd3eff2`, tests dfly `3bcf12f`).
  - `ttyu0` in `early_uart.c` is now a real tty. The RX FIFO drains into
    the line discipline from the UART interrupt (RX level, RX timeout,
    error bits → `TTY_FE/PE/OE/BI`). TX refills from the TX interrupt.
    Without a usable interrupt it falls back to a per-tick callout. The
    firmware's baud rate is kept.
  - `gic_fdt_irq()` turns a node's three-cell GIC `interrupts` entry into
    an MI irq and sets its trigger mode. `fdt_early` records the
    stdout-path node for it.
  - This is enough for an interactive console. The proper `dev/serial`
    driver stays in Phase 5.
- **4a done: static userland** (fork `0089c146ed`).
  - **Toolchain.** Stock host clang-18 + `tools/lld`, no clang patch yet.
    `tools/host/ubin/cc` (written by `bin/arm-world`) runs
    `clang --target=aarch64-unknown-dragonfly --sysroot=tools/sysroot` and
    supplies the OS predefines itself, since clang-18 has no aarch64
    DragonFly target info. The driver's DragonFly toolchain still picks
    `crt*.o`, `-lc` and `-lgcc` from the sysroot. The Clang patch above is
    still needed for a native compiler and for shared linking.
  - **Runtime.** `contrib/compiler-rt` (FreeBSD's `lib/builtins`,
    unmodified) built by `lib/libcompiler_rt` as `libgcc.a`;
    `lib/csu/aarch64` (crt1 adapted from x86_64, crti/crtn from FreeBSD,
    crtbegin/crtend). Neither is hooked into buildworld yet. The clang
    driver also links `-lgcc_eh`; there is no unwinder, so `arm-world`
    creates an empty `libgcc_eh.a`.
  - **libc MD** (`lib/libc/aarch64`): `SYS.h` + stubs (`svc #0`, carry =
    error), `cerror` with errno in static TLS, getcontext/makecontext,
    `rfork_thread`, setjmp family (FreeBSD), FP mode helpers, IEEE-quad
    long double classifiers, gdtoa glue (`strtorQ`, `machdep_ldisQ.c`),
    `static_tls.h`, `Symbol.map`. `lib/libc/thread/aarch64`: `pthread_md.h`
    and the `_umtx_*_err` stubs.
  - **MI touches**, both checked byte-identical on x86_64:
    `gen/tls.c` gets the TLS variant I layout (`[tcb][pad][TLS]`,
    `tpidr_el0` → tcb); `thread/thr_umtx.h` `cpu_pause()` is `yield` on
    aarch64.
  - **Headers.** OpenBSD's arm64 `ieee.h`/`fenv.h` in
    `contrib/openbsd_libm` (libc needs `machine/ieee.h`), `ieeefp.h`,
    `setjmp.h` with an `__ASSEMBLER__` guard, 16-byte aligned
    `mc_fpregs`.
  - **Kernel.** `exec_setregs` sets `MDP_EXECED`, and the syscall return
    path then leaves x0/x1 alone. Before, a successful `execve()` wrote 0
    over the new image's x0 (the stack pointer for `_start`), so every
    exec'd program crashed in `_start`. pid 1 is exec'd outside a syscall
    and never saw it. `ARM64_VIRT` gains `_KPOSIX_PRIORITY_SCHEDULING`
    (`sched_yield`).
  - **Built so far:** libc, libutil, libcrypt, csu, libgcc; `/bin/sh`
    (`BOOTSTRAPPING=1`: `-DNO_HISTORY`, pregenerated sources) and
    `/sbin/init`, all static.
  - **Tests** (dfly `tools/arm-smoke`):
    - `libctest.c`: ctors, stdio + quad long double, TLS errno, malloc,
      setjmp/signals, ucontext, fork/pipe/wait, pthreads + `__thread`.
      Passes as init and under sh.
    - `sh.exp`: init → single-user → sh, 21 expect steps (arithmetic,
      parameter expansion, loops, pipes, functions, `cd`/`pwd`, running
      libctest, ^C out of a busy loop). Passes.
    - sigtest and ttytest still pass.
- **Host tooling** (dfly):
  - `bin/arm-sysroot [DIR]` stages `/usr/include` into `tools/sysroot`,
    taking the file lists from the makefiles (`bmake -V`). `rpc/` and
    `rpcsvc/` headers come from the host's `rpcgen`.
  - `bin/arm-world DIR [targets] [-j2] [VAR=val]` runs the fork's
    makefiles with bmake for aarch64. Objects go to
    `tools/uobj/<fork path>`, and `install` goes into the sysroot. It is
    static only (`NOSHARED`, `NOPIC`) with `NO_NLS` (no BSD gencat).
    GNU install's missing flags are filtered out.
  - Recipe for an interactive root:
    ```sh
    bin/arm-world lib/csu/aarch64; bin/arm-world lib/libcompiler_rt
    bin/arm-world lib/libc -j2; bin/arm-world lib/libutil; bin/arm-world lib/libcrypt
    bin/arm-world bin/sh obj all BOOTSTRAPPING=1; bin/arm-world sbin/init obj all
    # root dir with sbin/init, bin/sh, dev/, etc/, tmp/ (stripped), then:
    bin/arm-mkiso sh.iso ROOT
    tools/arm-smoke/vmexpect.py -w 90 tools/arm-smoke/sh.exp -- \
        -s 2 -r sh.iso -a "vfs.root.mountfrom=cd9660:md0"
    ```
- **Gotchas:**
  - When libc runs as pid 1 its thread init does `setsid`,
    `revoke("/dev/console")` and `TIOCSCTTY` (`thr_init.c`). That revokes
    descriptors init opened earlier. libctest forks a child for the
    tests when it is pid 1.
  - DragonFly's `getcontext()` clears `uc_link`.
  - `nan()` is in libm, which isn't built yet.

#### Progress 4b (2026-10-03)

- **libm** (fork `80f16a01c5`, test dfly `45749b9`).
  - IEEE quad long double comes from OpenBSD's `src/ld128` and
    `arch/aarch64/fenv.c`, imported into `contrib/openbsd_libm`.
  - Bugs fixed in the ld128 code:
    - `LDBL_IMPLICIT_NBIT` / `LDBL_MANH_SIZE` were never defined, so
      `truncl` and `remquol` were wrong.
    - `remquol` lost the sign and the integer bit.
    - `cbrtl` was scaled wrongly.
  - New `ld128/e_sqrtl.c`: the generic version needs a quad division that
    honours rounding modes, and compiler-rt's doesn't.
  - `libmtest.c` checks all of it, static and shared.
- **rtld and shared libraries** (fork `aef36a195a`).
  - `libexec/rtld-elf/aarch64` (`reloc.c`, `rtld_machdep.h`,
    `rtld_start.S`) is adapted from FreeBSD, with TLSDESC (static, dynamic,
    undefined-weak), IRELATIVE and variant-PCS PLT slots.
  - `RTLD_IS_DYNAMIC()` is the constant 1. Before self-relocation the GOT
    slot for `_DYNAMIC` is still 0, and the MI test then skipped rtld's
    own relocation.
  - **TLS variant I in MI `rtld.c`:**
    - `allocate_tls`/`free_tls` keep the TCB at the start of the static
      block.
    - Module offsets start at `roundup2(16, align)`, and
      `tls_static_space` counts the TCB.
    - The TCB alignment follows the largest static module.
    - x86_64 code paths are unchanged, except that `reloc_plt()` takes
      `flags` and `lockstate`.
  - **libc_rtld:**
    - C string fallbacks;
    - aarch64 `setjmp` objects;
    - `longjmperror`;
    - `cerror.S` stores a plain global errno there.
  - **libc.so** links `-lgcc_pic`, since compiler-rt's quad helpers are
    hidden and each DSO gets its own copy. `libcompiler_rt` builds PIC and
    hidden, and installs `libgcc_pic.a` as a link to `libgcc.a`.
  - **Built shared:** `libc.so.8`, `libm.so.4`, `ld-elf.so.2`.
- **Host tooling** (dfly):
  - `ARM_SHARED=1 bin/arm-world …` drops `NOSHARED`/`NOPIC`.
  - lld gets `--undefined-version`, because the symbol maps name symbols
    some builds lack.
  - Shims: `tsort` dedups, which matters because libc lists its syscall
    objects twice. `install` ignores `-f`/`-b`; `chflags` is a no-op; `ln`
    maps `-h` to `-n`.
  - Absolute symlinks installed into the sysroot are rewritten as
    relative ones. Otherwise lld silently fell back to `libc.a`.
- **Tests** (dfly `tools/arm-smoke`):
  - `dltest.c` + `dltestlib.c` (`build-dltest.sh`):
    - TLS of a `DT_NEEDED` library and of a dlopen()ed one, across
      threads, including a thread that started before the dlopen;
    - dlclose and reopen;
    - dlsym/dladdr;
    - a lazy PLT bind with all argument registers live.
  - `run-dyn.sh WORK` builds a root with static init and sh plus dynamic
    dltest, libctest and libmtest, and runs `dyn.exp`, both lazy and with
    `LD_BIND_NOW`. All 19 steps pass.
  - `dlsym()` returns a function's definition, not the executable's
    canonical PLT entry, as on FreeBSD. So `dlsym(RTLD_DEFAULT, "f") != f`
    when the program takes `f`'s address.
- **Recipe:**
  ```sh
  ARM_SHARED=1 bin/arm-world lib/libcompiler_rt clean all install
  ARM_SHARED=1 bin/arm-world lib/libc clean all install -j2
  ARM_SHARED=1 bin/arm-world lib/libm clean all install -j2
  ARM_SHARED=1 bin/arm-world libexec/rtld-elf
  tools/arm-smoke/run-dyn.sh WORK      # needs sh and init built (4a recipe)
  ```
- **Not yet x86-checked:** the MI rtld changes (`rtld.c` variant I under
  `#ifdef`, the `reloc_plt` signature, the `rtld_lock.c` include) and
  `rtld_libc.c`. Run `bin/arm-x86build` with the next batch of MI
  changes.
- **The rest of `lib/`** (2026-10-03): every `lib/` subdirectory builds and
  installs shared. Fixes along the way:
  - **Headers:** DragonFly's own `<stddef.h>`, `<float.h>` and friends must
    win over clang's resource headers, the same way DragonFly's gcc (which
    ships none) sees them. Sources rely on what they pull in, such as
    `<sys/cdefs.h>`. The cross `cc` uses `-nobuiltininc -idirafter
    <resource>/include`. `include/float.h` hard-coded x87's 80-bit long
    double; it now has the binary128 values on aarch64, checked against the
    compiler's predefines.
  - **Lib-specific fixes:**
    - librecrypto: `OPENSSL_NO_ASM` off x86_64.
    - liblzma: `immintrin.h` only on x86.
    - libevtr: an aarch64 `va_list` built over the saved argument block.
    - libkvm: `kvm_aarch64.c`, no crash dumps yet; `kvm_proc.c` includes
      `<machine/pmap.h>`.
    - libefivar: `MDE_CPU_AARCH64`, and `machine/efi.h` from FreeBSD's
      arm64 (an ABI header).
    - ncurses: on a Linux host, the host-built tic uses ncurses' own getcap,
      since glibc has no `cgetent`.
  - **`arm-world`:**
    - creates the sysroot tree from `etc/mtree`;
    - passes `_SHLIBDIRPREFIX`, and its link rewrite handles links that
      already name the sysroot;
    - adds shims for `c++` and `rpcgen`;
    - adds a `tools/host/hinc` for host programs (`xlocale.h`,
      `__DECONST`).
  - **`arm-kbuild`** runs one bmake per target, because `-j2 depend all`
    raced the `machine/` forwarding headers.
- **Unwinder:**
  - `contrib/libunwind` is LLVM libunwind from FreeBSD's vendor tree, used
    unmodified. `lib/libgcc_eh` builds it, plus compiler-rt's
    `gcc_personality_v0`.
  - `libgcc_pic.a` is now the linker script `INPUT(-lgcc -lgcc_eh)`, so
    shared links get the unwinder as well.
  - `tools/arm-smoke/ehtest.c` tests `_Unwind_Backtrace`,
    `_Unwind_ForcedUnwind` with cleanups, and unwinding through libc's
    `qsort`. It passes static and dynamic in `run-dyn.sh`, now 27 steps.
- **SMP memory ordering (§3.7 follow-through):**
  - Under QEMU, dltest hung about once in a hundred runs. Three fixes:
  - **Atomics were not full barriers.** A SEQ_CST `LDAXR`/`STLXR` is not
    a full barrier, so a later plain load could pass it. That broke the
    IPI handshake: the receiver clears `gd_npoll` and then reads
    `ip_windex`, which can lose an IPI. Every `atomic.h` read-modify-write
    now ends in `dmb ish`, the x86 `lock` semantics that MI code assumes.
  - **IPI FIFO release.** A compiler fence before `ip_rindex`/`ip_xindex`
    let the slot be reused before it was read. `lwkt_ipiq.c` now uses
    `atomic_thread_fence_rel()`, new in both `atomic.h`s: a compiler
    fence on x86 (unchanged code there) and `dmb ish` on aarch64.
    `atomic_store_rel_int` would have been wrong for this hot path,
    because it is a locked `xchg` on x86.
  - **cpusync.** The stage-2 release and the acks got
    `atomic_thread_fence_rel/acq()`. `cpu_lfence` is a real `lfence` on
    x86, which isn't wanted there.
  - Rule for MI ordering fixes: they must cost x86 nothing beyond
    compiler fences. DragonFly's IPI and token paths are built to have
    no contention, and arm64 bring-up must not tax x86 to get there.
  - Afterwards, 300 runs of dltest in a row had no hang.
  - **Stale per-cpu pointer in `doreti`.** Two dltest loops in parallel
    once froze the guest: no output for 40 minutes, not even from the
    loops' 30 s hang watchdogs. Other runs panicked with "lwkt_switch: Attempt to switch from a
    fast interrupt" from the idle thread.
    - The cause: `doreti` cached `gd = mdcpu` and then called `ast()`,
      which can resume the thread on the other cpu.
    - The loop then ran on that cpu's globaldata. It made non-atomic
      `gd_intr_nesting_level` updates that raced with it, and it cleared
      its deferred `RQF_TIMER`. The one-shot timer stays disarmed when
      the tick is lost.
    - The fix re-reads `gd` on every loop iteration. x86 does that for
      free, because `PCPU()` is `%gs`-relative.
    - Any C trap-return code that holds a `gd` across a possible switch
      has the same hazard.
  - The stress test is `EXP=stress.exp WAIT=1500 tools/arm-smoke/run-dyn.sh
    WORK -t 1600`: two loops of 200 dltests each, in parallel, with a
    30 s hang watchdog per run. It takes about 8 minutes under TCG and
    now passes repeatedly.
- **Panic backtraces without ddb:**
  - `platform/arm64/aarch64/backtrace.c` walks the frame-pointer chain
    and steps through exception trapframes.
  - Boot tunable `debug.panic_test=1|2` tests it.
  - `tools/arm-smoke/ksym.sh LOG` symbolizes the output.
- **buildworld hookup (fork `d504cd1f73`):**
  - On aarch64, `Makefile.inc1` builds `_startup_libsrt`
    (`lib/libcompiler_rt`, `lib/libgcc_eh`) in place of
    `gnu/lib/gcc80/*`.
  - No gcc is cross-built: the compiler is external clang.
  - `gnu/lib` skips gcc80/gcc120, and `lib/` lists the two directories.
  - `bmake -V` shows the x86_64 lists unchanged.
  - Untested as a real buildworld, which needs a DragonFly host. The
    cross-tools list still names GNU binutils.
- **NLS:**
  - `bin/arm-hosttools` builds the fork's own `usr.bin/gencat` for Linux.
    glibc's gencat writes an incompatible format; the BSD format is
    big-endian, so it is the same on any host.
  - `arm-world` passes `GENCAT=` and no longer sets `NO_NLS`.
  - libc's 26 catalogs build and install.
- **Clang target patch (`tools/llvm/`):**
  - `clang-18-dragonfly-aarch64.patch`, 4 files, +52/−8, against clang
    18.1.3.
  - The patch adds `DragonFlyBSDTargetInfo<AArch64leTargetInfo>`.
  - `__tune_i386__` is x86-only.
  - gcc80 paths are x86-only.
  - On aarch64, `/usr/include` comes before the resource headers, and the
    default C++ library is libc++.
  - Checked without a full LLVM build: only clangBasic and clangDriver are
    built (245 steps), linked to the host `libLLVM-18.so`, and
    `tools/llvm/dftest.cpp` prints the predefines and the driver jobs.
  - x86_64 output is unchanged.
  - See `tools/llvm/README.md`. The `cc` shim stays until a patched clang
    is installed.
- **The rest of world (fork `8906778a65`):**
  - Everything in `bin`, `sbin`, `libexec`, `usr.bin`, `usr.sbin`,
    `gnu/lib`, `gnu/sbin` and `gnu/usr.bin` cross-builds and installs
    into the sysroot (dynamic, `ARM_SHARED=1`), except `sbin/devd`.
  - `devd` is the only C++ program in base. It waits for a libc++ and
    libcxxrt import (FreeBSD's vendored copies), which a native toolchain
    needs anyway.
  - Left out on aarch64 because they are x86-only: `cpucontrol`,
    `fdcontrol`, `fdformat`, `fdwrite`, `kbdcontrol`, `lptcontrol`,
    `moused`, `mptable`, `nvmmctl`, `rndcontrol`, `vidcontrol`, and the
    NVMM headers. Also left out: gdb, binutils, gmp/mpfr/mpc and
    cc80/cc120, because the toolchain is external clang.
  - Code changes:
    - truss gets `aarch64-fbsd.c`, adapted from the x86_64 one
      (syscall number in `x8`, `__syscall` shifts by one, errors in
      `PSR_C`).
    - ktrdump builds an aarch64 `va_list` from the logged argument blob
      (every argument in an 8-byte stack slot).
    - newfs_msdos: floppy ioctls are x86-only.
    - grep's gnulib `__attribute_noreturn__` is emptied for clang.
    - getconf, top and xz get small `__aarch64__` cases.
  - Host tools (`bin/arm-hosttools`, all from the fork): byacc as `yacc`,
    because bison places prologues differently; `rpcgen`, whose output
    differs from glibc's in names; `unifdef`, needed for `bsdxml.h`; and
    `gencat`. A generated `hostcompat.h` supplies `__dead2`,
    `getprogname` and friends to glibc.
  - `arm-world` shims: `find -s` (sorted), `gcc` → the target `cc`
    (kdump's mkioctls), and `install -B/-f` accepted. The mtree pass
    also creates `/var`.
  - Order matters: run the `includes` stage before `dfregress`,
    `gnu/lib` (luks.h), `gnu/sbin` and `tzsetup`.
- **Bugs found by running the world (2026-10-03):**
  - **rtld put libc's TLS on top of the TCB** (fork `f16e16bbe2`). A
    main program without PT_TLS leaves module index 1 unused, so the
    first block went through `calculate_tls_offset(0, 0, …)`. That is
    offset 0 on variant I. `errno` and `_ThreadRuneLocale` then shared
    the TCB's words, `isdigit()` read a garbage rune table, and
    `tail -n 2` and `head -30` failed. The first placement now keys on
    `tls_last_offset == 0`, which is identical on x86_64.
  - **No `hw.machine`** (fork `91f2c9cb09`): `uname(3)` failed, so make
    did too. arm64 now has `hw.machine`, `hw.model` (from MIDR_EL1, also
    printed at boot), `hw.physmem`, `hw.usermem` and `hw.availpages`
    (`sysconf(_SC_PHYS_PAGES)`).
- **Phase 3 exit test under load: passed 2026-10-03.**
  - `tools/arm-smoke/run-load.sh WORK 150 1500`: `mkworldroot.sh` puts
    the installed world (`bin`, `sbin`, `lib`, `libexec`, `usr/*`, the
    shared libraries, dltest) into an md root. QEMU runs `-smp 4 -m 2G`,
    pinned with `taskset` to two host cpus, so the vcpus get preempted.
  - In single-user mode on tmpfs: 150 rounds of `make -r -j8` over
    `load.mk`. Each round is 16 jobs of awk, sort, gzip/gunzip, cmp,
    uniq and wc over 20000 lines, each result checked. Alongside them,
    two loops of 1500 dynamic dltest runs, each with a 30 s hang
    watchdog.
  - Result: `make-fail=0 hung-a=0 hung-b=0`. It ran 1 h 27 min under
    TCG, about 35 s per round.
- **Still open for 4b:**
  - `sbin/devd` (C++): done in 4c with libc++.
  - x86 check, done 2026-10-03 at fork `1496798ba5`:
    - `bin/arm-x86build build` + `boottest`: the x86_64 kernel builds
      and boots.
    - In h2dev, gcc 8.3 `-Werror` compiles every object of
      `libexec/rtld-elf`, `lib/libevtr` and `lib/liblzma`.
    - `float.h` passes static asserts against gcc's x86 predefines.
    - The rtld link (`-lc_rtld_pic`) and `libkvm` (master's
      `machine/pat.h`) need a master buildworld, which the 6.4 guest
      doesn't have. Recheck them with the first full x86 buildworld.
  - x86 check of the world changes, 2026-10-03 at fork `91f2c9cb09`:
    gcc 8.3 `-Werror` in h2dev compiles every object of ktrdump,
    newfs_msdos, grep, top, getconf, xz, truss and rtld-elf (with the
    TLS offset fix). xz's link wants master's libc (`pthread_create`),
    like rtld's. No MI kernel code changed since `1496798ba5`.

#### Progress 4c: installworld, multi-user, libc++ (2026-10-03)

- **Phase 4 exit test: passed.** The world builds from clean on the
  Linux host (`bin/arm-buildworld`, 726 directories in 18 min at `-j2`; `share/` installs serially because its `FILES` install targets race under `-j`). It installs into an
  image root (`bin/arm-installworld`), becomes a root-owned UFS image
  (`bin/arm-mkimg`), and boots on QEMU `virt` to multi-user with `sshd`.
  - `make buildworld` itself does not run on Linux: Makefile.inc1's
    bootstrap and cross-tools stages assume a DragonFly host.
    `arm-buildworld` runs the same stages with the host tools and
    `arm-world`: headers, `_startup_libs` (with `_startup_libsrt`),
    `_prebuild_libs`, `_generic_libs`, then everything, one directory at a
    time with `-j2`.
  - `tools/arm-smoke/run-multiuser.sh WORK`, `multiuser.exp`, 25/25 steps:
    - rc runs fsck and the mounts, generates host keys, and starts
      syslogd, devd, sshd, sendmail (dma) and cron;
    - getty logs root in on `ttyu0`;
    - the databases: `id operator`, setgid `dma` owned `root:mail`;
    - `devd` runs;
    - `cxxtest.cc` passes dynamic and static;
    - `ssh root@localhost` logs in over lo0 (no NIC until Phase 5).
- **Host tools** (`bin/arm-hosttools`), all from the fork and built
  against glibc:
  - makefs (FFS only, its cd9660/msdos/hammer2 back ends dropped);
  - `pwd_mkdb` and `cap_mkdb` with libc's db;
  - zic, localedef, mkcsmapper, mkesdb.
  - A shared shim (`bsdlib.h` + `hostlib.c`) supplies `fgetln`, `errc`,
    `reallocf`, `strtonum`, `setmode` and the `sys/endian.h` names.
  - Implicit declarations are now errors in host builds: one in makefs
    (pwcache) and one in pwd_mkdb (`dbopen`, hidden behind
    `__BSD_VISIBLE`) truncated pointers on LP64 and segfaulted.
- **installworld** (`bin/arm-installworld ROOT`):
  - distrib-dirs from `etc/mtree`;
  - the `install` target of each top directory, in Makefile.inc1's order;
  - `etc distribution`;
  - `etc` runs without `-j`, because `etc/sendmail` installs the same
    files twice and races.
  - The build is unprivileged. `arm-world`'s install shim logs each
    `-o/-g/-m` to `ROOT.metalog` (`ARM_DESTDIR=ROOT`).
  - New `arm-world` shims: `mtree -deU` (directories only), `cpdup`, and
    `uudecode` (python).
  - zic gets `ZIC_UG_FLAGS=`, so it doesn't look up `wheel` on the host.
- **Image** (`bin/arm-mkimg [-s SIZE] ROOT IMAGE`): writes an mtree spec
  for `makefs -F`:
  - Everything defaults to `root:wheel`, with the on-disk modes less
    group/other write.
  - Directories take the owners and modes from `BSD.*.dist`.
  - Files take them from the metalog; names resolve against the image's
    own `master.passwd` and `group`.
- **libc++** (fork `679daf21f6` import, `89b76ae2ac` port):
  - `devd` is the only C++ program in base. aarch64 has no gcc, so it has
    no libstdc++ either.
  - FreeBSD's vendored libcxxrt and libc++ 21.1.8 are imported into
    `contrib/libcxxrt` and `contrib/libcxx`, with the 111 llvm-libc headers
    that `charconv.cpp` needs (see `README.DRAGONFLY`).
  - `lib/libcxxrt` and `lib/libc++` are built only for aarch64:
    - `libc++.a` contains libcxxrt's objects;
    - `libc++.so` is a `GROUP` linker script, as on FreeBSD;
    - the headers go in `/usr/include/c++/v1`, where the patched clang
      driver looks.
  - DragonFly changes to contrib, marked "DragonFly customization":
    - DragonFly takes FreeBSD's ctype, xlocale and stdlib paths, since its
      libc has the same `_CTYPE_*`, `_DefaultRuneLocale` and xlocale.
    - FreeBSD's `__LONG_LONG_SUPPORTED` guards would otherwise hide
      `std::strtoll`, `llabs` and `wcstoll` on any OS but FreeBSD.
    - Atomic waits use `umtx_sleep(2)`/`umtx_wakeup(2)` on a 32-bit
      contention word (as Linux's futex does), rather than the generic
      polling backoff.
    - FreeBSD's clang-18 fallback for `is_nothrow_convertible` lacks two
      includes.
  - `__config_site` sets `_LIBCPP_HAS_THREAD_API_PTHREAD`, because libc++
    doesn't detect DragonFly.
  - The `c++` shim uses `-stdlib++-isystem <sysroot>/usr/include/c++/v1`.
    `-nostdinc++` still overrides it, so libc++'s own build and the
    unwinder are unaffected.
- **Fork changes for installworld** (`f62cde7168`):
  - `etc/etc.aarch64` (`ttys` with getty on `ttyu0`–`3`, `ifconsole`;
    `disktab`).
  - libnvmm, libvgl and syscons' fonts and keymaps are x86-only.
  - `share/terminfo`: tic writes into the objdir, where install reads,
    instead of the source tree.
  - `rc.d/syscons` does nothing without `/dev/ttyv0`.
- **x86 check** (2026-10-03, fork `89b76ae2ac`):
  - The new `lib/` and `share/` entries are gated to one architecture.
    `bmake -V SUBDIR` for x86_64 `lib/` is identical to `91f2c9cb09`.
  - `share/terminfo` built and installed in h2dev with the guest's tic:
    2780 compiled entries in the objdir, none in the source tree, and
    all 2620 `TERMINFO_ENTRIES` installed.
  - `rc.d/syscons` is unchanged where `/dev/ttyv0` exists. On a
    serial-only console such as h2dev's, it is now silent rather than
    failing.
  - No kernel or libc changes.

### Phase 5 — FDT, newbus and generic devices (≈3–5 wk)

- **Import FreeBSD's OFW/FDT layer:**
  - `dev/ofw` (`openfirm.c`, `ofw_fdt.c`, `ofw_bus_subr.c`, `ofwbus.c`;
    skip `ofw_standard.c`): about 4K lines.
  - `dev/fdt` (`simplebus.c`, `fdt_common.c`): about 1.5K lines.
  - `libfdt`.
  - The kobj interface files (`ofw_if.m`, `ofw_bus_if.m`).
  - Adapt to DragonFly newbus. DragonFly lacks `BUS_PASS_*` multi-pass
    attach, so either port `bus_generic_new_pass` or use explicit
    `DRIVER_MODULE` ordering plus an early-attach hook for the GIC, timer,
    mbox and clocks.
- **Interrupt-parent / `interrupts-extended` resolution (§3.3)** and
  `dma-ranges` parsing feeding busdma tags (§3.5).
- **Minimal `dev/clk` / regulator / hwreset / syscon:** only what EMMC2 and
  GENET actually reference. FreeBSD `clk.c` is 1.7K lines; consider a stub
  that answers fixed rates from the firmware mailbox instead.
- **Generic drivers:**
  - PL011 as a proper `dev/serial` driver (DragonFly has `sio`; port
    FreeBSD's `uart_dev_pl011.c` + `uart_core` or write a small `pl011` tty
    driver).
  - `pci_host_generic` (ECAM) for QEMU `virt`, so DragonFly's existing virtio
    PCI, AHCI and xhci drivers work on QEMU.
  - PSCI reset/poweroff.
- **busdma rework (§3.5),** validated with virtio and xhci on QEMU. QEMU is
  coherent, so also add a debug knob that poisons or flushes caches
  aggressively to catch missing syncs before real hardware does.

**Exit:** QEMU `virt` with PCIe ECAM, virtio-net, virtio-blk/AHCI, and
qemu-xhci + usb-storage works. Run the HAMMER2 RAID6 test suite from
`../hammer2-raid6/tests` on four virtual disks in the guest, and it passes.

#### Progress 5a: FDT newbus and virtio-mmio (2026-10-03)

- **The newbus tree comes from the DTB.** On QEMU `virt` the installed
  world boots with root on a virtio-mmio disk, gets a DHCP lease on
  `vtnet0` and fetches a file from the host intact, on 1 and 2 CPUs (fork
  `00bba6225d`, `47b19fd616`).
  - `tools/arm-smoke/run-virtio.sh WORK`, `virtio.exp`, 17/17 steps:
    - root on `vbd0`;
    - lease `10.0.2.15` from QEMU's user network;
    - 4 MB fetched from a web server on the host, sha256 checked;
    - 8 MB written to and read back from a second virtio disk.
  - `run-multiuser.sh` (md root) still passes 25/25.
- **`sys/bus/ofw`, from FreeBSD, FDT only:**
  - `openfirm.c` merges `openfirm.c` and `ofw_fdt.c`. It calls libfdt
    directly, so there is no `ofw_if` kobj backend. A phandle is a node
    offset.
  - The xref list is under a spinlock.
  - `OF_init(fdt_va())` runs at `SI_BOOT1_POST`.
  - `ofw_bus_subr.c`, `ofw_bus_if.m` and `ofwbus.c` are adapted to
    DragonFly newbus:
    - rid by pointer;
    - an IRQ resource carries its cpu (`machintr_legacy_intr_cpuid`);
    - no `BUS_PASS`, since the GIC and timer are already up by SYSINIT.
  - DragonFly's `kfree(NULL)` panics, so property buffers are freed
    through the NULL-safe `OF_prop_free()`.
- **`sys/bus/fdt/simplebus.c`:**
  - FreeBSD's version, minus the MSI and `get_property` methods.
  - `reg` is translated through `ranges`.
  - `ofwbus` is `DEFINE_CLASS_1` of it.
- **`platform/arm64/aarch64/nexus.c`,** from the x86_64 nexus:
  - per-cpu irq rmans set up by the GIC's `MachIntrABI.rman_setup`;
  - memory resources mapped with `pmap_mapdev()` (Device-nGnRE);
  - one child, `ofwbus0`.
  - Interrupts: no INTRNG. `ofw_bus_map_intr()` propagates up to nexus,
    which maps the GIC's 3-cell SPI/PPI specifier to an MI irq
    (INTID − 16) and programs the trigger mode.
- **`autoconf.c`:** `root_bus_configure()` at `SI_SUB_CONFIGURE`. After
  it, `safepri` drops to `TDPRI_KERN_USER`, as on x86.
- **virtio-mmio:**
  - Upstream's legacy (v1) transport (`116009a77f`) gets an FDT
    attachment, `virtio_mmio_fdt.c` (`"virtio,mmio"`).
  - MI: `virtio_blk` and `vtnet` register on `virtio_mmio` as well as
    `virtio_pci`. Upstream had the transport but no driver attached to it.
  - QEMU fills the 32 slots from the top, and they attach from the
    bottom, so the last `-device` is unit 0.
  - `bin/arm-vm` takes extra QEMU arguments in `ARM_VM_ARGS`.
- Smaller fixes:
  - `support.c` gains `_bcopy` and friends (bpf).
  - `ARM64_VIRT` gets `bpf`, `virtio`, `virtio_mmio`, `virtio_blk` and
    `vtnet`.
- **Fixed in 5c: the contigmalloc DMA reserve was empty on `virt`.**
  `vm_page_startup` reserved absolute PFNs 0–65535, and `virt`'s RAM
  starts at 1 GB, so every `contigmalloc` took the general scan.

#### Progress 5b–5d: ECAM, busdma, AHCI, xhci + umass (2026-10-03)

- **5b, PCIe ECAM** (fork `e6c8a569a1`):
  - `pci_host_generic{,_fdt}.c` and `ofw_pci.h` from FreeBSD. The bridge
    keeps I/O, memory and prefetch rmans in PCI address space and maps
    child BARs itself through the FDT `ranges`. I/O space is
    memory-mapped, so I/O-port resources carry the memory bus tag.
  - INTx through the node's `interrupt-map`. MSI returns `ENXIO` until
    GICv2m is wired up; drivers fall back to INTx.
  - `run-virtio.sh -p` runs the virtio test on virtio-blk-pci and
    virtio-net-pci: 17/17, and mmio still passes 17/17.
- **vm: the contig reserve is based on the start of RAM** (fork
  `e4dd3af165`, MI). The alist is indexed from `vm_contig_base`, the
  first RAM page rounded down to the alist's 65536-page span (0 on PCs).
  On `virt`: "DMA space used: 9572k, remaining available: 130112k".
- **5c, busdma** (fork `b5e31e1c51`), `aarch64/busdma_machdep.c` from the
  x86_64 one:
  - Maps are never NULL, because the MI `bus_dmamap_sync()` macro skips
    NULL maps.
  - A load records its physical chunks, bounce pages included. Sync
    cleans and invalidates them to PoC through the DMAP. The partial
    lines at the ends of a POSTREAD are cleaned, not dropped.
  - `bus_dmamem_alloc()` memory is contiguous and Normal non-cacheable
    (KVA and DMAP alias), so rings that drivers never sync work.
  - `hw.busdma.coherent=1` turns cache maintenance off.
  - `hw.busdma.debug` catches missing syncs on QEMU, which models no
    caches. Bit 0 audits each load's PRE/POST sequence
    (`hw.busdma.audit_reports`). Bit 1 poisons the buffer at PREREAD.
    Both accept USB's `usb_pc_cpu_invalidate` idiom, a POSTREAD and
    then a PREREAD after which the CPU reads the buffer.
  - Not yet: bouncing buffers that share cache lines with other data,
    and `dma-ranges` offsets. Both matter on the Pi 4.
- **5d, AHCI and xhci + umass** (fork `590d70f716`). Nothing sets up PCI
  on `virt`: BARs are unassigned and bus mastering is off. MI fixes, none
  of which changes PCs:
  - ahci enables bus mastering, as FreeBSD's does.
  - `pci.c` skips USB early takeover when the controller's BAR has no
    resource list entry. The takeover's lazy BAR allocation left a stale
    range, and xhci's own attach failed with "Could not map memory".
  - xhci no longer steps the control status stage (FreeBSD's
    `XHCI_STEP_STATUS_STAGE`). QEMU fetches a control transfer as one
    chain and never starts one whose status TRB is held back, so no
    device enumerated.
  - virtio_pci falls back to the legacy interrupt when MSI-X allocation
    fails, instead of panicking.
  - `ARM64_VIRT` gets `ahci`, `scbus`, `da`, `pass`, `usb`, `xhci` and
    `umass`.
  - `tools/arm-smoke/run-storage.sh WORK`, `storage.exp`, 19/19: root on
    virtio-blk-pci, an AHCI disk (`da0`) and a usb-storage disk on
    qemu-xhci (`da8`; umass disks are numbered from da8). Booted with
    `hw.busdma.debug=3`, each disk gets 32 MB written and read back, and
    the audit stays at 0.
- **x86 check pending:** the MI changes (`DRIVER_MODULE` virtio_mmio,
  `pci_pci.c`, `pci.c`, `virtio_pci.c`, ahci, xhci, `vm_page.c`) need a
  build and boot on h2dev with `bin/arm-x86build`.
- Later: MSI through GICv2m.

**Progress 5e (2026-10-03): Phase 5 exit test passed.**
- **RAID6 overlay on master.** `hammer2-raid6/src` moved from v6.4.2 to
  master (hammer2-raid6 `7e24841`). The three-way merge conflicted in
  `hammer2_io.c` (master replaced the dio RB tree with a spinlocked hash;
  the RAID6 path now uses it), `newfs_hammer2.c`, `hammer2_flush.c` and
  `hammer2_ioctl.c`. `hammer2-raid6/bin/apply-overlay SRCDIR` installs the
  overlay into a master tree; `hammer2_raid6.patch` (`e3f3c9a`) is the
  same thing as a patch against `arm64-base`. It no longer applies to
  6.4.2, so h2dev's `deploy.sh`/`install_raid6.sh` flow needs that guest
  on master. Done on arm64 directly, not on x86 first as planned: the
  overlay has not been built on x86 against master yet.
- **Upstream kdmsg race fixed** (fork `52d1c56856`, `kern_dmsg.c`).
  Plain single-disk hammer2 hung on master under TCG, with or without the
  overlay: the `hammer2 service` daemon's startup scan sent RECLUSTER for
  the new mount before it `accept()`ed mount_hammer2's connection, and the
  reconnect waited for the old reader thread while holding the root vnode
  lock; that reader was blocked on a socket nobody would accept or close.
  The reconnect now shuts down the old descriptor itself. Fast x86 wins
  the race, which is why it doesn't show there. Found with a temporary
  lockmgr timeout that printed the holder's acquire-site PCs and
  `td_wmesg`.
- **Test:** `tools/arm-smoke/run-raid6.sh [-n] WORK` with `raid6.exp`.
  It needs a fork worktree with the overlay applied (`R6SRC`), its
  kernel built as `ARM64_R6`, and `sbin/newfs_hammer2` + `sbin/hammer2`
  built with `DFLY_SRC=R6SRC bin/arm-world DIR obj all -j2`. Boots the
  installed world plus 4 × 4 GB virtio-blk-pci disks and runs
  `tests/v3/run_all.sh`. `R6GROUPS=` picks groups (not `GROUPS`, a bash
  builtin). Result: **65 pass, 0 fail**, groups A–L (incl. resilver, EIO
  injection, scrub, read-path self-heal), no panics.
- **x86 check pending** now also covers `kern_dmsg.c` and the overlay
  built against master.

### Phase 6 — Raspberry Pi 4 bring-up (≈6–10 wk)

Order matters: console, then SD, then USB, then network.

1. **Boot and console.**
   - Put `kernel8.img` with the Image header on the FAT partition, with
     `config.txt` as in §3.2 stage B.
   - Use a USB-TTL serial adapter on GPIO14/15.
   - Stack: PL011, GICv2 (low-peri addresses from DT), generic timer at
     54 MHz.
   - Spin-table SMP.
   - Use an md root at first.
2. **Firmware mailbox and firmware property driver** (`bcm2835_mbox.c`,
   `bcm2835_firmware.c`): clock rates, power domains, board revision and
   MAC address.
3. **EMMC2 SD:** DragonFly `dev/disk/sdhci` plus FreeBSD's `bcm2711-emmc2`
   attachment. Respect the 1 GB DMA window on B0 silicon (from DT
   `dma-ranges`). Then root on SD (UFS or HAMMER2).
4. **PCIe + VL805 + USB 3:**
   - Port `bcm2838_pci.c`, keeping its 960 MB inbound clamp. It includes the
     internal MSI controller as a PIC for `gic_abi`.
   - The mailbox call for `notify_xhci_reset` comes before xHCI attach
     (`bcm2838_xhci.c`).
   - DragonFly's u4b `xhci` + `umass` then gives us USB disks. Test UAS
     enclosures separately: DragonFly u4b UAS support is limited, so use
     BOT/umass at first.
5. **GENET:**
   - Port `if_genet.c` to `sys/dev/netif/genet/`. Adapt it to DragonFly's
     ifnet: `ifq` serializers, `IFNET_SERIALIZE_ALL`, `if_start` vs
     `if_transmit`, and mbuf API differences.
   - Add the BCM54213PE PHY via `mii` (`brgphy` may already match it;
     otherwise add the ID).
   - The MAC address comes from the DT `local-mac-address` (firmware fills it
     in) or the mailbox.
6. **Housekeeping:**
   - GPIO for the activity LED as a disk/heartbeat indicator.
   - `bcm2711-rng200` feeding `kern_nrandom`.
   - Watchdog, for `reboot` via PM_RSTC or PSCI.
   - Thermal sensor (`brcm,bcm2711-thermal`) for the FlyNAS dashboard.
   - cpufreq via the mailbox: optional.
7. **8 GB board:** run with the RAM above 960 MB enabled and confirm that
   bounce buffers work under sustained USB load. **Use `md5`/`b3sum` over
   large files.** Silent corruption is the failure mode FreeBSD hit.

**Exit:** the Pi 4 boots multi-user from SD, gets a DHCP lease on GENET,
`sshd` works, and a 4-disk HAMMER2 RAID6 volume on a USB 3 hub mounts,
scrubs and survives a pulled disk.

### Phase 7 — Proper boot chain and install (≈3–4 wk)

- Port `stand/boot/efi/loader` to aarch64:
  - `arch/aarch64/{start.S, exec.c, ldscript}`.
  - The FDT is taken from the EFI config table and passed as
    `MODINFOMD_DTBP`.
  - Kernel ELF load and the `modulep` handoff.
- Firmware: U-Boot `rpi_4` (EFI), or pftf EDK2 in DT mode.
  - Pros of pftf: PSCI and SMCCC via TF-A (needed for the Spectre-v2
    mitigation), and a normal ESP.
  - Cons of pftf: its 3 GB toggle, and its default of ACPI mode.
  - Choose one and document it in `docs/`.
- Kernel modules on aarch64 (`kmod.mk` link mode, `link_elf_obj.c`
  relocations `R_AARCH64_*`).
- A `release/` script that produces an SD image: FAT (Pi firmware +
  `config.txt` + U-Boot or EDK2 + `EFI/BOOT/BOOTAA64.EFI`) followed by a
  DragonFly root. HAMMER2 root needs `vfs.root.mountfrom` support from the
  loader.

**Exit:** `dd` a release image onto an SD card, boot it, and run a normal
DragonFly install onto a USB disk.

### Phase 8 — Packages and FlyNAS (≈4–8 wk)

- FlyNAS needs these packages from `install.sh`: `openresty`, `git`,
  `sqlite3`, `lua51-cjson`, `libargon2`, `dnsmasq`, `qemu`, `cdrtools`.
- **DPorts** is x86_64-only, and its bulk-build tool (`synth`) is written in
  Ada, so bringing up an Ada compiler first is a non-starter. Two options:
  - **(a) Build the handful of ports natively** on the Pi from DPorts with
    plain `make install` / `pkg create`, and host a tiny private `pkg` repo.
  - **(b) Cross-build** with a qemu-user-static chroot. This doesn't work: we
    have no DragonFly user-mode emulation.
  - Choose (a). Short-term, extend `install.sh` to install from the private
    repo.
- **Per-package notes:**
  - OpenResty's LuaJIT supports arm64. Check LuaJIT's OS detection for
    `__DragonFly__` combined with `__aarch64__`, since its `lj_arch.h` has
    per-OS/arch tables.
  - `lsqlite3`, `cjson` and `argon2` are plain C.
- **FlyNAS changes:**
  - **VM apps:** NVMM is x86-only. Hide the VM/app catalog on arm64, or
    gate it on `sysctl hw.machine_arch` plus the presence of NVMM in
    `flynas-helper.c` (`QEMU_CMD` is hardcoded to `qemu-system-x86_64`).
    Revisit if someone ever ports a hypervisor (ARM EL2 and VHE are not on
    the A72 anyway).
  - **Dashboard:** CPU temperature from the bcm2711 thermal sysctl, and SMART
    via USB-SATA bridges (`smartctl -d sat`).
  - `bin/` harness: an `arm` target (a QEMU `virt` image and a real Pi over
    ssh).
  - The UI is WASM and is already architecture-neutral.
- **HAMMER2 / RAID6:** our `local_hammer2_raid6.c` and the rest of
  `hammer2-raid6/src/sys` contain no x86 intrinsics or asm (checked with
  grep), so they should compile as-is.
  - Watch endianness assumptions (none expected: little-endian both sides),
    alignment of on-disk structs read via casts (aarch64 tolerates unaligned
    normal-memory access), and the cost of xxhash64/CRC on the A72.
  - Optional later: NEON GF(2^8) multiply for RAID6 Q parity using the
    split-nibble `tbl` lookup (plain ASIMD; PMULL is not available on the
    BCM2711), via `fpu_kern_enter`.

**Exit:** `install.sh` completes on the Pi, the full `tools/uitest/run-all.sh`
suite passes against it (minus VM-app tests), and the bitrot self-heal test
set (A–L in `../hammer2-raid6/docs/bitrot.md`) passes on USB disks.

### Phase 9 — Hardening and performance (open-ended)

- Spectre-v2/BHB: SMCCC `ARCH_WORKAROUND_1` on exception entry from EL0
  (needs TF-A firmware), plus the BHB loop for the A72.
- Lazy FPU switching, superpage promotion, and relaxing atomics from
  sequentially consistent to acquire/release where proven safe.
- Raise the PCIe DMA window toward 3 GB once we understand the 960 MB
  corruption that FreeBSD reported. Bounce copying is the throughput ceiling
  for USB disks on 4 GB and 8 GB boards.
- An `ddb` aarch64 disassembler, plus `kgdb` and `minidump` support.
- Upstreaming: offer the MI cleanups and then the port to DragonFly (and
  claim the bounty).

### Effort summary

| Phase | Weeks |
|---|---|
| 0 Tooling + MI cleanups | 1–2 |
| 1 Headers, hello world | 2–3 |
| 2 pmap/traps/irq/timer to `init` | 8–12 |
| 3 SMP | 3–4 |
| 4 Userland/toolchain | 6–10 (overlaps 2–3) |
| 5 FDT/newbus/busdma | 3–5 |
| 6 Pi 4 drivers | 6–10 |
| 7 Boot chain/install | 3–4 |
| 8 Packages + FlyNAS | 4–8 |
| **Total** | **≈ 30–50 engineer-weeks** (consistent with the 2012 estimate of 1200–2000 hours) |

---

## 4a. Throughput budget: 1 GbE from a 4-SSD RAID6 over USB 3

Target: saturate 1 GbE, which is about 112–117 MB/s of TCP payload. All
numbers below are estimates or published Pi 4 measurements, not measurements
of this port. **Phase 6 must measure them.**

| Stage | Load at 117 MB/s | Ceiling on Pi 4 | Verdict |
|---|---|---|---|
| SSD reads (healthy RAID6, 4 disks = 2 data + P + Q) | ~60 MB/s per data disk | ~300–350 MB/s per USB SSD | Fine |
| VL805 / PCIe Gen2 x1, shared by all 4 ports | 117 MB/s read; **~234 MB/s on write** (data + P + Q) | ~3 Gbps ≈ 375 MB/s aggregate in published tests | Reads fine, writes OK (~60%) |
| Bounce copies (RAM above 960 MB) | One extra memcpy of 117 MB/s | Several GB/s of memory bandwidth | Fine: a few % of one core. Needs a large enough bounce pool (16–32 MB) so I/O doesn't stall. |
| Cache maintenance (`dc civac` per buffer) | 117 MB/s | — | Fine |
| HAMMER2 xxhash64 check on read | 117 MB/s | Well above 1 GB/s on an A72 core | Fine |
| RAID6 degraded read, 1 disk lost (P/XOR) | 117 MB/s | XOR is cheap | Fine |
| RAID6 degraded read, 2 disks lost (Q, GF(2^8)) | 117 MB/s | Scalar table code is probably a few hundred MB/s | Probably fine; NEON `tbl` if not |
| GENET + TCP | 1 Gbps | Linux reaches ~940 Mbps on a Pi 4 | Fine, provided the DragonFly GENET driver supports checksum offload and has decent ring sizes |
| **Encryption** | 117 MB/s | **No AES instructions.** Software AES-GCM is well below 117 MB/s per core. ChaCha20-Poly1305 with NEON is several hundred MB/s. | **Bottleneck.** See below. |

**Conclusion:** the 960 MB DMA limit costs CPU, not bandwidth. A plain
protocol (HTTP without TLS, NFS, SMB without encryption, rsync) should
saturate 1 GbE. Encrypted transfers will not, unless the cipher is
ChaCha20-Poly1305:

- **SSH/SFTP:** OpenSSH already defaults to ChaCha20-Poly1305.
- **OpenResty/HTTPS:** order ChaCha20-Poly1305 suites first for clients
  without AES hardware. A large download over AES-GCM will cap well below
  line rate.
- **SMB3 encryption:** AES only, so it won't reach line rate.
- **Write path:** watch it. 234 MB/s to the disks plus read-modify-write on
  partial stripes is the tightest margin in the table.
- **Hardware:**
  - Use a single powered hub. Ports share 1.2 A, and four SSDs need their
    own power.
  - Use BOT-capable USB-SATA bridges. UAS is not needed at these rates.

**Measure first.** Before writing any Pi drivers, boot Raspberry Pi OS on the
actual board with the actual hub and SSDs, and record:

| Tool | Measures |
|---|---|
| `fio`, 4 disks in parallel | VL805 aggregate |
| `iperf3` | GENET |
| `openssl speed -evp aes-128-gcm` and `-evp chacha20-poly1305` | Cipher cost |
| `mdadm` RAID6 + `fio` | Software RAID ceiling |

That gives hardware ceilings to hold the DragonFly drivers against, and it
takes about a day.

Raising the PCIe inbound window from 960 MB to 3 GB (Phase 9) makes bounces
rare on a 4 GB board. It is a CPU optimisation, not a requirement for 1 GbE.

## 4b. Xen as an alternative foundation

AMD's Versal Xen is upstream Xen plus Xilinx patches for Versal-specific
hardware: SMMU-500, GICv3, EEMI firmware calls and the programmable logic.
None of that hardware exists on the BCM2711. Upstream Xen has supported the
Pi 4 since **4.14** (2020), including the 4 GB and 8 GB boards (the fix there
was keeping enough dom0 memory below 1 GB for the DMA-limited devices). So
the question is really whether to use upstream Xen.

The A72 runs Xen at EL2 without needing VHE. The Pi has **no IOMMU**, so
device passthrough to guests isn't safe. All guest I/O goes through
paravirtualised (PV) drivers in dom0.

| Shape | What DragonFly needs | Saves | Costs |
|---|---|---|---|
| **A. Linux dom0, DragonFly domU running FlyNAS** | Core port (Phases 0–4 unchanged: pmap, traps, IRQ, SMP, userland). Xen ARM guest glue (HVC hypercalls, event channels on a PPI, grant tables, xenstore, PV console). blkfront and netfront from FreeBSD `sys/dev/xen`, which is BSD-licensed but **not wired for arm64 in FreeBSD**, so the arch glue is new. | Most of Phases 5–6: PCIe/VL805/xHCI, GENET, EMMC2, non-coherent busdma, the 960 MB bounce problem. Linux dom0 handles all of them. | Linux becomes a mandatory layer under the NAS. SMART, disk identity and hot-plug live in dom0. PV I/O costs CPU on 4 slow cores. FlyNAS can only manage the box through an agent in dom0. |
| **B. DragonFly dom0** | Everything in the native plan, **plus** Xen backend drivers (blkback, netback), xenstored and the libxl/`xl` toolstack | Nothing | Strictly more work than the native port |
| **C. Native (current plan)**, VM apps dropped or added later | As in §4 | — | No "apps as VMs" on arm64 |

**Effect on "apps as VMs":** shape A is the only cheap way to get this
feature back on ARM. App VMs become additional domUs. They must be arm64
images (the current catalog is x86 under NVMM), and FlyNAS would drive
`xl create` in dom0 through a small privileged agent. The alternative is to
give DragonFly an EL2 hypervisor backend (NVMM-on-ARM). That is a project as
large as this port.

**Recommendation:** keep the native port (shape C) as the plan of record.
Xen doesn't touch the hardest parts (pmap, deferred interrupts, TSO
assumptions, the toolchain). It trades the Pi driver work for PV-driver work
plus a permanent Linux dependency. Revisit shape A only if VM apps on the Pi
become a hard requirement, or if Phase 6 driver bring-up stalls. The
blkfront/netfront path would then be the fallback that still delivers a
working NAS.

## 4c. VM apps on arm64: NVMM backend + microVM VMM

The idea is Fly.io-style microVMs: OCI/Docker arm64 images turned into a
rootfs and booted as tiny Linux VMs. This would replace the x86-only
NVMM + QEMU app path. It splits into two independent projects.

### 1. NVMM arm64 backend (kernel)

**This is the hard part.**

DragonFly's NVMM is structured for multiple backends:

- `nvmm.c` (~1K lines) is the MI core. OS glue is `nvmm_dragonfly.c`.
- Each backend implements `struct nvmm_impl` (`nvmm_internal.h`):
  `ident`, `machine_create`/`configure`, `vcpu_create`/`setstate`/
  `getstate`/`run`, and so on.
- Today the only backends are `x86/nvmm_x86_{vmx,svm}.c` (~7K lines).
- The backend table in `nvmm.c` is `#if defined(__x86_64__)`.

A new `nvmm_arm64.c` needs:

- **An EL2 world switch.** The A72 has no VHE, so this is split ("nVHE")
  mode: a small EL2 runtime with its own page tables, saving and restoring
  the EL1 system registers around each guest run. It depends on the Phase 1
  hyp-stub.
- **Stage-2 page tables** (`VTTBR_EL2`, with a VMID) mapping guest-physical
  addresses to host pages. Hook them into the aarch64 pmap, just as the x86
  backends use EPT/NPT pmaps.
- **A virtual timer:** `CNTVOFF_EL2` and `CNTHCTL_EL2`, with PPI injection.
- **A vGIC v2** using the GIC-400's virtualisation interface (GICH at
  `0xff844000`, GICV at `0xff846000`, maintenance interrupt PPI 9). The Pi
  has no GICv3.
- **Exits to userland:**
  - MMIO data aborts. When `ESR.ISV` is set, the syndrome already gives the
    register, size and direction. Unlike x86 there's no need for the 3.5K-line
    instruction emulator in `libnvmm_x86.c`.
  - `hvc`/`smc` for guest PSCI (vCPU on/off).
  - `wfi`.
  - System-register traps.
- **API:** an `nvmm_arm64.h` state/exit ABI, written fresh. The x86 ABI
  (`nvmm_x64_state`, I/O-port and MSR exits) doesn't carry over.

**Donor:** FreeBSD's arm64 bhyve kernel (`sys/arm64/vmm`, ~9.6K lines,
BSD-licensed). It already does nVHE (`vmm_nvhe.c`, `vmm_hyp_el2.S`),
stage-2 (`vmm_mmu.c`), the vtimer and the instruction-abort decoding. But
its vGIC is **v3 only** (`io/vgic_v3.c`), so a vGIC-v2 backend is new work.
Use Linux `arch/arm64/kvm/vgic/vgic-v2.c` as the hardware reference only.

**Estimate:** ≈ 12–20 weeks, after Phase 3 (SMP) is stable.

### 2. The VMM (userland)

**Option A — QEMU with a new `target/arm/nvmm` accelerator. Recommended.**

- Model it on `target/arm/hvf/hvf.c`. Apple's Hypervisor.framework has the
  same userland-split shape as NVMM (run → exit → userland emulates), and
  that file is ~2.5K lines.
- Pair it with a lean `-M virt` configuration (virtio-mmio/PCI, no
  firmware, `-kernel` direct boot). That gives most of the microVM benefit.
- QEMU is already a FlyNAS dependency, and on x86 `-M microvm` with
  `accel=nvmm` covers the same idea today.

**Option B — port Firecracker.** Not recommended. Firecracker is
Linux-only by design:

- It drives KVM through the rust-vmm `kvm-ioctls` crate, with no hypervisor
  abstraction layer.
- It uses epoll/eventfd/timerfd (→ kqueue) and TAP ioctls.
- Its security model (the jailer) relies on seccomp, cgroups and
  namespaces. DragonFly has none of them, so we'd be shipping Firecracker
  without the isolation that makes it Firecracker.
- Rust has no `aarch64-unknown-dragonfly` target. Only x86_64 DragonFly
  exists, and it is tier 3. We'd have to add the target, plus `libc` crate
  bindings.
- We'd carry a permanent fork.

If a Rust VMM is wanted later, start from Cloud Hypervisor instead. It
already abstracts the hypervisor behind a trait (KVM and MSHV backends), so
an NVMM backend slots in rather than replacing the core.

**Guests:** arm64 Linux kernels plus rootfs images built from OCI images.
The current x86 app catalog doesn't carry over.

**Pi 4 sizing:** 4 cores and at most 8 GB, shared with HAMMER2, its buffer
cache and OpenResty. Realistically that's a handful of small app VMs.

**Order of work:** finish the NAS (Phases 0–8) first. Then do the NVMM
backend, then QEMU's `nvmm` accelerator. To try the microVM app model before
any of that exists, use x86 (`qemu -M microvm -accel nvmm`) on existing
DragonFly hardware.

## 5. Risks and open questions

1. **x86 memory-ordering assumptions in MI code (TSO).** This is the most
   likely source of rare SMP heisenbugs. Mitigations: full-barrier atomics in
   v1, `INVARIANTS` + `WITNESS`-like token debugging, and long QEMU `-smp 4`
   soak runs. Note that QEMU TCG is *more* ordered than real hardware, so the
   real soak has to happen on the Pi.
2. **Missing busdma syncs in DragonFly drivers.** Every driver we reuse (u4b
   xhci/umass, sdhci, mii, virtio) has only ever run on coherent x86.
   Mitigation: the "cache-hostile" debug mode in Phase 5, and checksum-heavy
   tests in Phase 6.
3. **The PCIe inbound 960 MB vs 3 GB question.** It affects USB 3 throughput
   on boards with more than 1 GB. Unknown root cause.
4. **No `BUS_PASS` in DragonFly newbus.** FDT drivers depend heavily on
   attach ordering (clocks before consumers, interrupt controllers first).
   We may end up porting multi-pass attach.
5. **Upstream reception.** We need to decide early whether this is a FlyNAS
   private fork or aimed at upstream DragonFly. That changes how carefully we
   do the `MACHINE`/ABI naming, MI cleanups, and toolchain choices (an
   external LLVM vs vendoring). **Recommendation:** aim for upstream, and
   send the Phase 0 MI cleanups early to get a read on maintainer appetite.
6. **Tooling gaps:** no `ddb` disassembler, no `kgdb`. QEMU's gdbstub is our
   debugger until Phase 9. Real-hardware debugging is serial only (no JTAG
   unless we wire one to the Pi's GPIO JTAG pins with `enable_jtag_gpio=1`,
   which is possible and worth doing in Phase 6).
7. **Scope creep in FDT support code** (clk, regulator, pinctrl, syscon).
   Keep a strict "only what EMMC2, GENET and PCIe need" rule. Prefer
   firmware-provided fixed clocks.

---

## 6. Immediate next steps

1. ~~Create the DragonFly git fork and `arm64` branch.~~ Done: `flynas/dragonfly`,
   branch `arm64` (see Phase 0).
2. Land the MI x86-leak cleanups (§3.6) on x86_64 and verify with
   `bin/vm` / h2dev.
3. Write `bin/arm-vm` (QEMU `virt` + gdb) and the clang/lld kernel build
   wrapper.
4. ~~Do Phase 1: headers, `locore.S`, PL011 early console, banner on QEMU.~~
   Done 2026-10-02 (see Phase 1).
   Phase 2's exit test passed on 2026-10-02 (see Phase 2), and Phase 3
   (SMP) works on QEMU with up to 4 CPUs (see Phase 3). Signals and the
   Phase 2 hazards are done, and the MI changes build and boot on x86_64.
   The PL011 tty and Phase 4a (static `init` + interactive `sh`) are done
   (see Phase 4, Progress). Next:
   - Phase 4b is done, and so is the Phase 3 exit test under load (see
     Progress 4b).
   - Phase 4 is done (Progress 4c): buildworld from clean,
     installworld into a UFS image, and a multi-user boot with sshd.
   - Phase 5a is done (Progress 5a): FDT newbus, virtio-mmio disk and
     NIC. 5b–5d are done too (ECAM, busdma, AHCI, xhci + umass), and
     the Phase 5 exit test passed (Progress 5e: RAID6 suite 65/65 on
     master). Next: the x86 check of all MI changes in h2dev
     (`bin/arm-x86build`), then Phase 6 (Pi 4 bring-up).
5. In parallel, order hardware:
   - a Pi 4B (4 GB, C0 stepping preferred)
   - a 3.3 V USB-TTL serial cable
   - a powered USB 3 hub
   - 2–4 USB-SATA bridges known to work with BOT
   - a good 5 V/3 A supply

---

## Appendix A — DragonFly MD surface checklist

### A.1 pmap API used by MI code

| Group | Functions |
|---|---|
| Mapping | `pmap_enter`, `pmap_remove`, `pmap_remove_pages`, `pmap_remove_specific`, `pmap_protect`, `pmap_page_protect`, `pmap_kenter[_noinval]`, `pmap_kremove`, `pmap_qenter[_noinval]`, `pmap_qremove[_noinval]`, `pmap_map`, `pmap_copy`, `pmap_unwire` |
| Lookup | `pmap_extract[_done]`, `pmap_kextract`, `pmap_kvtom`, `pmap_phys_address` |
| Page state | `pmap_mapped_sync`, `pmap_ts_referenced`, `pmap_clear_modify`, `pmap_clear_reference`, `pmap_is_modified`, `pmap_zero_page[_area]`, `pmap_page_init`, `pmap_page_set_memattr` |
| Lifecycle | `pmap_pinit`, `pmap_pinit0`, `pmap_pinit2`, `pmap_release`, `pmap_puninit`, `pmap_reference`, `pmap_growkernel`, `pmap_init`, `pmap_init2`, `pmap_init_thread`, `pmap_init_proc`, `pmap_setlwpvm`, `pmap_replacevm` |
| Faults and objects | `pmap_fault_page_quick`, `pmap_emulate_ad_bits`, `pmap_prefault[_ok]`, `pmap_object_init[_pt]`, `pmap_object_free` |
| Counts and queries | `pmap_resident_tlnw_count`, `pmap_wired_count`, `pmap_mincore`, `pmap_invalidate_range`, `pmap_collect`, `pmap_pgscan`, `pmap_addr_hint`, `pmap_maybethreaded` |

Also: `struct md_page` (`vm_page.h:189`) and the per-pmap `copyin` function
pointers.

### A.2 Thread and switch

- Switch and restore: `cpu_lwkt_switch`, `cpu_heavy_switch`,
  `cpu_exit_switch`, `cpu_lwkt_restore`, `cpu_heavy_restore`,
  `cpu_kthread_restore`, `cpu_idle_restore`, `savectx`.
- Thread lifecycle: `cpu_set_thread_handler`, `cpu_fork`, `cpu_prepare_lwp`,
  `cpu_set_fork_handler`, `cpu_lwp_exit`, `cpu_thread_exit`.
- Return paths: `fork_trampoline`, `fork_return`, `generic_lwp_return`.

### A.3 Interrupts, IPIs and timers

- The `MachIntrABI` methods.
- Deferred-interrupt machinery: `doreti`, `splz`, `splz_check`,
  `setsoft*`, and the `gd_ipending` / `gd_spending` / RQF_* handling.
- IPIs: `cpu_send_ipiq`, `cpu_send_ipiq_passive`, `smp_invltlb`,
  `stop_cpus`, `restart_cpus`, `cpu_smp_stopped`, `cpu_sniff`.
- Timers: a `cputimer` and a `cputimer_intr` registration, plus the MI
  `cpu_cyclecount()` from §3.6.

### A.4 Traps, signals and ptrace

- Traps and syscalls: `trap`, `trap_pfault`, `syscall2`, `userenter`,
  `userret`, `userexit`.
- Signals: `sendsig`, `sys_sigreturn`, `sigcode`, `cpu_sanitize_frame`,
  `cpu_sanitize_tls`, `exec_setregs`.
- Debug and register access: `fill/set_regs`, `fill/set_fpregs`,
  `fill/set_dbregs`, `ptrace_set_pc`, `ptrace_single_step`.

### A.5 User access and copy routines

- `std_copyin`, `std_copyout`, `std_copyinstr`, `fu*`/`su*`, `casu32/64`,
  `std_swapu*`, `std_fuwordadd*`.
- `setjmp`/`longjmp`, `bcopy`/`bzero`/`memcpy`.

### A.6 Machine bring-up and misc

- `initarm` (the `hammer_time` equivalent), `cpu_reset`, `cpu_halt`,
  `cpu_idle`, `identcpu`, `cpu_spinlock_contested`.
- `busdma_machdep.c`, `nexus.c`, `autoconf.c`, `elf_machdep.c` (relocations
  for kernel modules and rtld), `lwbuf.c`, `in_cksum`.
- Build glue: `ldscript.aarch64`, `kern.mk` (`-mgeneral-regs-only`,
  `-ffixed-x18`, no red-zone concerns on arm64).
