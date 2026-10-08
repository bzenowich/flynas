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
forward-ported to master, runs on 4 virtio disks (Progress 5e). The
rewritten suite (2026-10-04) runs 123/0 on arm64 `-smp 2`, over both
virtio-mmio and virtio-pci, and the MI changes pass the x86 check
(Progress 5f). The 2026-10-05 review (`review-10-05.md`) found
ordering, stub and durability bugs QEMU cannot show; its first batch of
fixes is in Progress 5f. The Pi groundwork that QEMU can test is in
Progress 5g: DMA windows and bouncing, interrupt spreading, faster copy
routines, async parity writes, pulled-disk handling, RTC write-back, and
dntpd setting the clock at boot. The watchdog and RNG drivers are
written. The Phase 6 drivers (mailbox, PCIe, GENET + PHY, EMMC2,
thermal) are written and compile-tested (Progress 6a). `bin/arm-pisd`
makes the SD image, and DHCP + sshd management is tested (Progress 6b).
Next: boot on the board.

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
  - **DDB was off in ARM64_VIRT** until `db_interface.c`, `db_trace.c` and `setjmp`
    existed (done 2026-10-05, see "Still open from Phases 1–5"). `machdep.c` keeps a non-DDB
    `Debugger()` for kernels without DDB, because HAMMER2 calls it
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
- ~~**Not done:** an x86_64 rebuild in h2dev for the two MI header changes.~~
  Done with the Phase 2 x86 check of `6bfd5cc205` (2026-10-02).

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
  - ~~All SPIs still go to cpu 0.~~ Spread over the cpus since
    `db037a5749` (Progress 5g).
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
    - libkvm: `kvm_aarch64.c` (reads minidumps since 2026-10-05); `kvm_proc.c` includes
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
- **Minimal `dev/clk` / regulator / hwreset / syscon** (done 2026-10-05,
  `bus/fdt/fdt_res.c`): only what EMMC2 and
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
- ~~**x86 check pending:**~~ (done 2026-10-04 at `52d1c56856`, Progress 5e)
  the MI changes (`DRIVER_MODULE` virtio_mmio,
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
  6.4.2. Done on arm64 directly, not on x86 first as planned.
- **h2dev moved to master (2026-10-04).** The x86 guest now runs
  `48147b0412` + overlay, world and kernel built in the guest from
  `/usr/src` (branch `master-h2`) with `KERNCONF=H2DEV` (X86_64_GENERIC
  minus `options HAMMER2`, keeps hammer2 a module); 6.4.2 kernel kept as
  `/boot/kernel.old`, VM snapshots `pre-master` / `master-base`.
  buildworld 45 min + buildkernel 45 min at -j2. `NO_ALTCOMPILER=yes`
  must go to installworld/upgrade as well as buildworld. `deploy.sh`
  now syncs via `apply-overlay` (hammer2-raid6 `df83818`). Full v3 suite
  on x86: 64/1, the one failure a dmesg(8) ENOMEM race in the test (fixed
  with a retrying `kmsg` helper, `7f79a22`; E+L then 11/11).
- **x86 check of the fork passed (2026-10-04).** `bin/arm-x86build build`
  of fork `52d1c56856` (all MI changes since `1496798ba5` plus the
  `kern_dmsg.c` fix) on the master guest: clean, 17 min at -j2;
  `boottest` booted it with no panic/lock-order/witness lines and
  returned to the stock kernel. On that kernel, 5 cycles of
  mount/write/umount/remount/verify of one newfs'd single-disk hammer2
  with `hammer2 service` running all passed (the kdmsg race path).
  Installed kernels are now stripped (`strip --strip-debug`) to fit
  `/boot`; the debug copies stay in the obj trees.
- Upstream master side finding: `timeout(1)` fails at start on both the
  stock master kernel and the fork kernel with `sigaction(32): Invalid
  argument` (it catches every signal below `sys_nsig`, SIGTHR included),
  after it has already forked the child. Not investigated further.
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
- ~~**x86 check pending** now also covers `kern_dmsg.c` and the overlay
  built against master.~~ Done: the fork check above covers `kern_dmsg.c`,
  and the overlay runs 123/0 on x86 (Progress 5f).

**Progress 5f (2026-10-04 – 10-05): suite rewrite, two arm64 bugs, review fixes.**
- **The 65/65 above was vacuous.** An audit against mdadm's tests found
  checks that could not fail. `hammer2-raid6/tests/v3` was rewritten
  (groups A–N), which exposed kernel bugs that were all fixed, and the
  2 GB volume limit went away (space map + v3 bulkfree). The rewritten
  suite runs **123 pass, 0 fail** on x86 and on arm64 `-smp 2`.
- **pmap wire bug** (fork `9e9f1a4a9a`). `pmap_enter` took the page wire
  only for managed mappings, so pages `kmem_alloc`'d wired before
  `pmap_init` (the pv zone's boot store) had wire count 0. Group N's
  24 GB churn let the pagedaemon free one, and file data overwrote live
  pv entries (panic in `pmap_remove`). A software PTE bit
  (`ATTR_SW_PGWIRED`) now records that the PTE holds a wire, as on x86.
- **"PCI INTx with SMP" was a vtblk bug** (fork `d9ccd244ad`, MI).
  vtblk sized its queues from the MSI-X count and demanded one vector per
  queue. With no MSI controller on arm64 the legacy fallback gives one
  vector, so attach failed with ENXIO. vtblk now shrinks to the vectors it
  gets. Suite with `R6BUS=pci -s 2`: 123/0. The x86 check passed (x86
  gets full MSI-X, so it does not take the shrink path).
- **Review fixes, batch 1** (`review-10-05.md` §6 item 1 and R1):
  - `sysarch(2)`, `cpu_set_iopl`/`cpu_clr_iopl` return `EOPNOTSUPP` and
    `md_dumpsys` prints that dumps are unsupported; none panic any more.
  - Ordering (O1–O5): `cpu_sfence`/`cpu_mfence` are `dmb osh` and
    `cpu_lfence` is `dmb oshld`. x86 is TSO, so MI code uses
    `cpu_sfence` as a release barrier, and DMA devices sit in the outer
    shareable domain. `bus_space` writes start with `dmb oshst`, which
    orders Normal-memory stores (descriptors) before a doorbell write.
    `bus_space_barrier` uses `dsb`. `load_acq`/`store_rel` get the
    missing `dmb ish`. `_bus_dmamap_sync` issues `dsb sy` for
    PREREAD/PREWRITE/POSTREAD even when no cache maintenance is needed
    (Normal-NC dmamem). MI: xhci reads the event TRB only after its
    cycle bit (`cpu_lfence`); `sys_pipe` and `lwkt_switch_return` fence
    before the stores that publish.
  - **USB flushes (R1).** `da` used to set `DA_Q_NO_SYNC_CACHE` for every
    umass device, so `BUF_CMD_FLUSH` did nothing on USB disks. It now
    sends SYNCHRONIZE CACHE, and sets the quirk with a console warning
    only when the device rejects it (ILLEGAL REQUEST or
    `CAM_REQ_INVALID`); that flush then completes as success. A bridge
    that hangs instead needs the loader tunable
    `kern.cam.da.umass_sync_cache=0` (the old behaviour).
    `kern.cam.da.N.sync_cache` shows and sets the state, and the new
    `DIOCGFLUSHCAP` ioctl reports it. HAMMER2 flushes each member at a
    read-write mount, asks `DIOCGFLUSHCAP`, and warns about members that
    cannot flush.
  - Overlay (R2, R3): the per-disk fsync loop keeps the first error. A
    RAID6 member that fails to sync is auto-failed, so the headers still
    go to the healthy members. P/Q, zero-fill and rebuild-column write
    errors now reach `hammer2_raid6_write_failed()`: a healthy member is
    auto-failed, and a write error on the disk being resilvered fails the
    resilver.
  - Commits: fork `e53e96d01f` (stubs), `6159fec48b` (arm64 ordering),
    `d9182c584f` (pipe, lwkt), `6ff70b9e26` (xhci), `a559cf9187` (da);
    hammer2-raid6 `3c5c48f` (R2/R3), `6ede151` (mount warning),
    `b368139` (patch).
  - Tests, arm64 `-smp 2`: the RAID6 suite (`R6BUS=pci`) 123/0;
    `run-storage.sh` (busdma audit 0); and the new
    `EXP=tools/arm-smoke/flush.exp USBSIZE=1g run-storage.sh -n WORK --
    -s 2 KERNEL`, 31/31. That test mounts hammer2 on the usb-storage disk
    with flushes working (no warnings), then forces umass's
    `UQ_MSC_NO_SYNC_CACHE` with `usbconfig add_dev_quirk_vplh` and
    re-attaches. `da` and hammer2 must warn, and the data must survive a
    remount. The x86 check of these MI changes passed later, with
    5g's.

**Progress 5g (2026-10-05): review §6 item 3, the Pi groundwork QEMU can test.**
- **R5, DMA limits** (fork `ad8d0da4f7`):
  - `busdma_fdt.c` parses a bus node's `dma-ranges`. simplebus and the
    FDT PCIe bridge return a windowed tag from `bus_get_dma_tag`.
  - Tags carry a bus offset, which a load adds to each segment.
    `lowaddr` stays a physical limit, so bounce pages and dmamem come
    from below it.
  - Tags with a NULL parent (most DragonFly drivers) get the strictest
    window in the tree as `lowaddr`, or the tunable `hw.busdma.lowaddr`.
  - `MAX_BPAGES` is now 4096 (16 MB per zone).
  - Test: `bounce.exp` with `run-storage.sh`, using `KENV=hw.busdma.lowaddr=...`
    or `DTB=` made by `fdt-addprop.py`. AHCI and usb-storage bounce
    thousands of pages, the data matches, and the sync audit stays at 0.
  - Untested: a non-zero bus offset (QEMU has none).
- **S1/P3, interrupts** (fork `db037a5749`):
  - SPI n goes to cpu n % ncpus, and `intr_setup` points its
    ITARGETSR byte at the cpu that holds the handler.
  - `hw.gic.irq_balance=0` restores the old routing, and
    `hw.gic.irq.N.cpu` pins one irq.
  - `intr.exp`, `-smp 4`: virtio, ahci and xhci are taken on cpus 0, 1
    and 2.
  - MSI (the brcmstb controller) is part of Phase 6 step 4.
- **P1, copy routines** (fork `7c1650325b`): `memcpy.S` runs 64-byte
  `ldp`/`stp` loops, and `copyin`/`copyout` run 64-byte `ldtr`/`sttr`
  loops. `copytest.exp` checks every alignment 0–15 against every length
  0–300 plus large ones, `memmove` in both directions, and faults on an
  unmapped page.
- **P2 and R4, overlay** (hammer2-raid6 `db7d0f2`):
  - P/Q, mirror and zero-column writes are started together
    (`hammer2_bwrite_start`) and waited for once.
  - The flush checks each member vnode's write-error count, fails a
    member whose delayed writes returned EIO, and invalidates that
    member's buffers.
  - `pull.exp` (`EXP=` for `run-raid6.sh`, which uses `ARM_VM_MONITOR`)
    has QEMU `drive_del` a member 3 s into a 200 MB copy. The copy and
    sync must finish, the disk must be FAILED, and the file must match,
    both cached and from a degraded remount: 28/28.
  - The test passes on the old overlay too: the parity writes, which
    were already checked (R3), catch the pulled disk. The new scan is a
    backstop that no test reaches yet.
- **Time** (fork `63d6773139`, `1bc64454c4`):
  - `inittodr` starts a stopped PL031 and never sets the clock earlier
    than the root fs time. `resettodr` writes the PL031 back, and must
    use `nanotime()`, because `getnanotime()` is stale right after
    `set_timeofday()`.
  - `rc.d/savetime` (off by default) steps a board without an RTC (the
    Pi) forward to the time saved at shutdown.
  - `dntpd -s` was an `XXX`. It now queries the servers before
    daemonizing, steps to the median of up to 3 offsets, and gives up
    after `-w` seconds (15), leaving the step to the daemon. `-s` is the
    default flag, and `rc.d/dntpd` runs right after NETWORKING (before
    `mountcritremote` and syslogd), so the services start with the time
    from pool.ntp.org (`dntpd.conf`'s `N.dragonfly.pool.ntp.org`).
  - `time.exp`: the PL031 keeps a set date across a reboot, and savetime
    steps forward (23/23).
  - `ntp.exp` uses `fakentp.c`, a fake server on localhost, with
    `RCCONF=` (`run-storage.sh`). The step lands before syslogd starts,
    and an unreachable server times out and still daemonizes (20/20).
  - Untested: the real pool and DHCP on boot (no network in the
    sandbox). Cosmetic: the step message appears twice on the console.
- **Watchdog and entropy** (fork `1f260d2c87`):
  - `bcmwd` (BCM2835 PM watchdog) registers with the wdog framework
    (15 s maximum) and provides `cpu_reset_hook` for `reboot` without
    PSCI.
  - `bcmrng` feeds the RNG200 FIFO to the csprng as `RAND_SRC_RNG200`.
  - Both are compile-tested only; they attach on the Pi.
  - The license comments in `dump_machdep.c` and `sysarch.c` were not
    closed (fork `c7d8a7f9a2`).
- **Suite:** the RAID6 suite on the final kernel (`R6BUS=pci -s 2`):
  123 pass, 0 fail, no panics.
- **x86 check passed** (2026-10-05, fork `1bc64454c4`):
  - `X86_64_GENERIC` with all modules builds with gcc 8 in h2dev and
    boots. This covers the MI kernel changes of 5f and 5g: `random.h`,
    xhci, da, pipe and lwkt.
  - `dntpd` builds with WARNS=6 and `-Werror`. The rc scripts pass
    `sh -n`, and rcorder puts dntpd right after NETWORKING.
  - The x86 run of the overlay suite was not repeated; the arm64 suite
    covers P2/R4.
  - h2dev's 12.6 GB root filled up during the build. Old crash dumps
    and `/usr/obj` were deleted to make room.

**Progress 5h (2026-10-05 – 10-06): DDB, crash dumps, NEON RAID6, modules.**
All four items from the "Wanted early" and "Planned work" lists below are
done; details are under each item there.
- DDB (fork `36de9246c1`): `ddb.exp` 53/53.
- Crash dumps (fork `f95b22cfd2`, MI USB fix `c5f71a54bd`): `dump.exp`
  34/34.
- `kernel_fpu_begin`/`end` (fork `97d400bcaf`) and NEON RAID6 parity
  (hammer2-raid6 `2215aaf`, patch `e39e239`): groups A–E, K, L pass
  with `vfs.hammer2.raid6_simd=1`.
- Kernel modules (fork `32b778cd9c`, `497385bdd7`): all of
  `sys/modules` builds; `kmod.exp` 31/31.
- x86 check passed 2026-10-06 at fork `497385bdd7`: `bin/arm-x86build
  build` (X86_64_GENERIC with all modules, 16 min; the usb module
  regenerated its own `usb_if.c`) and `boottest`.

#### Still open from Phases 1–5 (audited 2026-10-05)

Every exit test from Phase 0 through Phase 5 passed. These items were
planned or found along the way and are not done. Code-level items were
checked against the fork at `1bc64454c4`.

**Blocks Phase 6:**
- ~~Minimal clk / regulator / hwreset / syscon (Phase 5).~~ Done
  2026-10-05 (`95b5a62304`, `bus/fdt/fdt_res.c`, `fdt_syscon.c`):
  providers register under their phandle; consumers resolve
  `clocks`/`clock-names`, `resets`/`reset-names`, `*-supply` and syscon
  phandles. Built in: `fixed-clock`, `fixed-factor-clock`,
  `regulator-fixed` (a GPIO-switched one only if always-on/boot-on), and
  a generic `syscon` driver. No clock tree or rate propagation; the Pi's
  firmware clock provider (mailbox) is Phase 6 work. `fdtres.exp` (DTB
  from `mk-res-dtb.sh`, `debug.fdtres_test=1`) passes 24/24 checks.
- MSI and a second-interrupt-controller layer (Phase 5). The MSI half
  is done for GICv2m (2026-10-05, `b949b806bd`): the generic ECAM bridge
  passes MSI/MSI-X to `gic.c`, virtio-blk-pci takes one MSI-X vector per
  queue and AHCI an MSI (`msi.exp` 19/19; `intr.exp` now sees 4 cpus).
  The second-controller layer is done too (`65f3c9b141`,
  `intr_cascade.c`): inputs of a controller behind one GIC SPI get their
  own MI irqs from the SPIs the device tree leaves unused, are
  demultiplexed in `intr_dispatch()`, and can be named as DT interrupt
  parents. `intrc.exp` (a software controller, `debug.intrc_test=150`)
  passes 23/23. Not shown by it: that the critical-section fires took
  the pend-and-replay path. Still open: the brcmstb MSI controller
  itself (Phase 6 step 4, Pi only).
- ~~Bouncing of buffers that share cache lines (5c, review O6).~~ Done
  2026-10-05 (`6638e1b964`): every tag on a non-coherent system bounces
  pieces that are not aligned to the cache writeback granule
  (`hw.busdma.bounce_edge`, `dma_align`, `edge_bounces`). `edge.exp`
  passes 25/25 under `hw.busdma.debug=3`; umass bounces its CBW and CSW on
  every command.

**Wanted early on the Pi (debugging):**
- ~~DDB (Phases 1 and 2).~~ Done 2026-10-05 (fork `36de9246c1`):
  `options DDB` and `DDB_TRACE` in `ARM64_VIRT`, so a panic prints a
  backtrace and drops into ddb.
  - Breakpoints are `brk #0`; the compiled-in `breakpoint()` is `brk #1`.
    Single step uses MDSCR_EL1.SS. Faults inside ddb recover through
    `db_nofault`. The other cpus are stopped while ddb runs.
  - Traces follow the frame-pointer chain through exception frames to
    user mode, for the current thread or `trace/t TD`.
  - `db_disasm.c` is a full A64 integer and system decoder. It matches
    llvm-objdump-18 on every instruction in the kernel. SIMD/FP, LSE
    and SVE print as `.word`.
  - Symbols: with no loader, `conf/mkksyms.sh` appends `.symtab` and
    `.strtab` to `kernel.bin` after the bss, minus the `$x`/`$d` mapping
    symbols. initarm hands them to link_elf as an "elf kernel" preload
    record (MODINFOMD_SSYM/ESYM), so static functions get their names.
    `kernel.bin` now stores the bss as zeros, growing from 5.6 MB to 14 MB.
  - Test: `ddb.exp` with `run-storage.sh -- -s 2` passes 53/53. It
    covers entry, registers, trace, `x/i`, a fault on address 0, `ps`,
    a breakpoint hit on cpu1, step, delete, continue, and panic → ddb →
    `reset`.
  - Tests that panic now stop at a `db>` prompt instead of spinning. Both
    end at the VM timeout.
- ~~Crash dumps (Phase 2).~~ Done 2026-10-05 (fork `f95b22cfd2`): a
  minidump of the kernel page tables and every page in the `vm_page_dump`
  bitmap; `savecore` writes `vmcore.0` and libkvm (`kvm_aarch64.c`) walks
  the dumped L0–L3 tables from `kernl0pa`, so `dmesg -M` and `ps -M` work.
  - `dump_avail` is every DMAP range, taken before the kernel image and
    `pmap_bootstrap` pages are subtracted from `phys_avail`, or libkvm
    can't find the kernel's own translation tables.
  - With no loader, `parse_bootargs` supplies
    `kernelname=/boot/kernel/kernel` unless the bootargs name one, so
    `kern.bootfile` (and savecore's kernel match) is right.
  - **MI fix found along the way** (`c5f71a54bd`): a panic inside a sysctl
    handler hung the reboot, because `usb_shutdown` detaches the device
    tree and `sysctl_ctx_free` waits for the sysctl lock the panicking
    thread holds shared. `usb_shutdown` now returns early when
    `panicstr != NULL`.
  - Test: `dump.exp` (`run-storage.sh DUMPSIZE=512m ROOTSIZE=800m
    ARM_VM_REBOOT=1`) passes 34/34: panic, dump, reboot, savecore,
    `dmesg -M` and `ps -M` on the vmcore.
- PL011 as a proper `dev/serial` driver (Phase 5). Optional: the early
  tty in `early_uart.c` works on the Pi's PL011.

**Planned work not done:**
- ~~`fpu_kern_enter` (Phase 2).~~ Done 2026-10-05 (fork `97d400bcaf`) as
  DragonFly's `kernel_fpu_begin()`/`kernel_fpu_end()`: a critical
  section, `TDF_KERNELFP`, the thread's user FP state saved to its pcb and
  FPCR zeroed; `kernel_fpu_end` reloads it. The kernel still builds with
  `-mgeneral-regs-only`, so SIMD code is assembly
  (`.arch_extension simd`).
- ~~NEON RAID6 parity (review P5).~~ Done 2026-10-06 (hammer2-raid6
  `2215aaf`): parity and recovery are column-wise over two primitives,
  `hammer2_raid6_xor` and `hammer2_raid6_mul` (multiply-accumulate by a
  GF(2^8) constant). On aarch64 they run 64-byte NEON steps in
  `hammer2_raid6_neon.S` (split-nibble `tbl`), C for the tail. A boot
  self-test checks all 256 constants against C and falls back on a
  mismatch; `vfs.hammer2.raid6_simd` shows the choice. Tests: host math
  check 2256 recoveries vs the old code; arm64 guest with SIMD on, groups
  A–E, K, L pass. Not yet measured on real hardware (TCG timings mean
  nothing).
- ~~Kernel modules (Phase 2, deferred to Phase 7).~~ Done 2026-10-06
  (fork `32b778cd9c`, `497385bdd7`): `kmod.mk` links aarch64 modules
  `-r` like x86_64, and all 309 modules in `sys/modules` build
  (`bin/arm-kbuild -n ARM64_VIRT modules-depend modules -j2`).
  - Fixes: `kmod.mk` generated no `*_if.c` when the kernel objdir had
    one (MI; x86 only escaped by build order); `-m aarch64elf` for
    firmware blobs; AES-NI, `pcf`, `mxge`, `sym` x86_64-only; `re`'s
    CMAC bus tag. `bin/arm-hosttools` builds `uudecode` for the
    firmware modules.
  - Test: `kmod.exp` (`KMODS=1 ROOTSIZE=600m run-storage.sh ... -- -s 2`)
    31/31: msdos load, FAT mount, unload, reload, then 19 more modules
    loaded and unloaded (dm_target_crypt pulls in dm and crypto).
  - Build but can't load yet (missing subsystems): `smbacpi`,
    `gpio_acpi`, `gpio_intel`, `sdhci_acpi`, `ig4` (ACPI), `nataisa`
    (ISA PnP), `ukbd` (kbd), `uaudio` (sound).
- ptrace debug registers and `PT_STEP` (Phase 2); no gdb or lldb is built.
- `pmap_fault_page_quick` and `pmap_object_init_pt` are stubs (Phase 2).
  These are performance only.
- Lazy FPU switching (Phase 2, optional).
- `cpu_send_ipiq_passive` is declared but not implemented (Phase 3); MI
  code doesn't call it today.
- The ABI review against upstream (Phase 4, "before anything ships").
  `ucontext.h` and `tls.h` are still marked PROVISIONAL.
- A native toolchain (Phase 4). The clang patch was only tested at driver
  level, and no LLVM is built for the target. Phase 8 plan (a) needs it.

**Written but never run:**
- ASID rollover (`vm.pmap.asid_max`, Phase 3). Multi-user now makes this
  testable on QEMU.
- The spin-table AP start, EL2 entry and early cache clean (Phases 1 and
  3). These run only on the Pi.
- Core dumps (Phases 2 and 4): the register fill exists.
- A real `make buildworld` on a DragonFly host (Phase 4).
  `bin/arm-buildworld` stands in for it, and the cross-tools list still
  names GNU binutils.
- RAID6 on umass/xhci (Phase 5 exit test). It ran on virtio only.
- From 5g: a non-zero DMA bus offset, `bcmwd`, `bcmrng`, and the real NTP
  pool.

**x86 follow-ups:**
- The rtld link (`-lc_rtld_pic`), libkvm and xz's link need a full x86
  buildworld of the fork (4b). h2dev's master buildworld on 2026-10-04
  built upstream master plus the overlay, not the fork.
- `timeout(1)` fails at start with `sigaction(32)` on master and on the
  fork (5e). Not investigated.

**Dropped:** the full enumeration of missing machine headers (Phase 0
exit note). Phase 1 made it moot.

### Phase 6 — Raspberry Pi 4 bring-up (≈6–10 wk)

Target hardware (decided 2026-10-06): a stock Pi 4B booting from
microSD, 4–6 SSDs on a USB 3 hub through Sabrent USB/SATA adapters
(VIA VL715), management over Gigabit Ethernet only. No sound, HDMI, GPIO
or Wi-Fi/BT. The driver list below is cut to what that needs.

Order matters: console, then SD, then USB, then network.

1. **Boot and console.**
   - Put `kernel8.img` with the Image header on the FAT partition, with
     `config.txt` as in §3.2 stage B.
   - Use a USB-TTL serial adapter on GPIO14/15.
   - Stack: PL011, GICv2 (low-peri addresses from DT), generic timer at
     54 MHz.
   - Spin-table SMP.
   - Use an md root at first.
2. **Firmware mailbox, minimal** (`bcm2835_mbox.c`): only the property
   channel, for the VL805 `NOTIFY_XHCI_RESET` call in step 4. The MAC
   address comes from the DT, clock rates from the DT, and the firmware
   powers the blocks we use; no firmware-property driver. Written
   (`bcmmbox`, Progress 6a).
3. **EMMC2 SD:** DragonFly `dev/disk/sdhci` plus FreeBSD's `bcm2711-emmc2`
   attachment. Respect the 1 GB DMA window on B0 silicon (from DT
   `dma-ranges`). Then root on SD (UFS or HAMMER2). Written
   (`sdhci_bcm`, Progress 6a).
4. **PCIe + VL805 + USB 3:**
   - Port `bcm2838_pci.c`, keeping its 960 MB inbound clamp. It includes the
     internal MSI controller as a PIC for `gic_abi`.
   - The mailbox call for `notify_xhci_reset` comes before xHCI attach
     (`bcm2838_xhci.c`).
   - DragonFly's u4b `xhci` + `umass` then gives us USB disks. Test UAS
     enclosures separately: DragonFly u4b UAS support is limited, so use
     BOT/umass at first.
   - Written (`pci_brcmstb.c`, Progress 6a), INTx only; MSI is still
     to do.
5. **GENET:**
   - Port `if_genet.c` to `sys/dev/netif/genet/`. Adapt it to DragonFly's
     ifnet: `ifq` serializers, `IFNET_SERIALIZE_ALL`, `if_start` vs
     `if_transmit`, and mbuf API differences.
   - Add the BCM54213PE PHY via `mii` (`brgphy` may already match it;
     otherwise add the ID).
   - The MAC address comes from the DT `local-mac-address` (firmware fills it
     in) or the mailbox.
   - Written (`if_genet.c` and `brgphy` delay support, Progress 6a).
6. **Housekeeping:**
   - ~~GPIO for the activity LED~~: dropped (no GPIO use on this NAS).
   - `bcm2711-rng200` feeding `kern_nrandom`: written (`bcmrng`,
     Progress 5g); check that it attaches and harvests.
   - Watchdog, for `reboot` via PM_RSTC or PSCI: written (`bcmwd`,
     Progress 5g); check `reboot` and a watchdog timeout.
   - Time: with no RTC, enable `savetime` and check that `dntpd -s` sets
     the clock from the pool at boot (Progress 5g).
   - Thermal sensor (`brcm,bcm2711-thermal`) for the FlyNAS dashboard:
     written (`bcmtemp`, Progress 6a).
   - ~~cpufreq via the mailbox~~: dropped; the firmware sets the clock
     (`config.txt` if it turns out too low).
   - ~~Mini-UART~~: dropped; `dtoverlay=disable-bt` puts the PL011 on
     GPIO14/15.
7. **8 GB board:** run with the RAM above 960 MB enabled and confirm that
   bounce buffers work under sustained USB load. **Use `md5`/`b3sum` over
   large files.** Silent corruption is the failure mode FreeBSD hit.

**Progress 6a (2026-10-06): the Phase 6 drivers, written and
compile-tested** (fork 168f903f5f). None of them can run under QEMU,
which has no BCM2711 model. They are first exercised on the board.
New kernel config `ARM64_RPI4`; `ARM64_VIRT` is unchanged.
- **Mailbox** (`dev/misc/bcmmbox`):
  - Polled, property channel only, with one 256-byte coherent buffer
    under a lock. Its bus address comes from the `/soc` `dma-ranges`
    (phys 0 is bus 0xC0000000).
  - Calls: `notify_xhci_reset`, plus `get_clock_rate`, which EMMC2
    uses only as a fallback.
- **PCIe** (`bus/pci/pci_brcmstb.c`, a subclass of the generic FDT
  host):
  - Resets the RC, opens an 8 GB inbound window, and waits up to
    200 ms for link.
  - Sets up the one outbound window, and programs the root port's bus
    numbers and memory window itself (DragonFly's `pci_pci` only reads
    them).
  - If the VL805 shows no firmware, it calls `notify_xhci_reset` before
    the `pci` child attaches, so `xhci` needs no Pi-specific code.
  - Child DMA is clamped below 960 MB (tunable `hw.bcm_pcib.dma_limit`).
  - INTx only for now; MSI returns ENXIO.
- **GENET** (`dev/netif/genet`):
  - DragonFly ifnet: arpcom, serializer interrupt, `if_start` and a
    watchdog.
  - One RX and one TX ring on queue 16, with no checksum offload.
  - `phy-mode` becomes `MIIF_RX_DELAY`/`MIIF_TX_DELAY` for the PHY.
  - The MAC address comes from the DT, then the UMAC registers, then a
    random locally administered one.
  - Fixed relative to FreeBSD:
    - A TX mbuf (and its map) is kept with the last descriptor.
    - The RX error and no-mbuf paths advance the consumer index.
    - RX loads into a spare map first.
- **PHY** (MI: `miidevs`, `miivar.h`, `mii.c`, `brgphy.c`):
  - BCM54213PE is matched by `brgphy`.
  - `mii_probe_args.mii_flags` now reaches the PHY; it was dropped
    before.
  - New `MIIF_RX_DELAY`/`MIIF_TX_DELAY` flags set the BCM54xx RGMII RX
    skew and GTXCLK delay.
- **EMMC2** (`dev/disk/sdhci/sdhci_fdt_bcm.c`):
  - DragonFly's sdhci core with SDMA, through the windowed tag, so the
    emmc2bus `dma-ranges` apply.
  - 32-bit register access, with block and command shadowing (as
    FreeBSD does).
  - UHS and 1.8 V signalling are off (the vqmmc regulator is not
    driven), so cards run at High Speed, 50 MHz.
  - `broken-cd` means the card is always present.
  - `mmc` attaches to it (one line in `mmc.c`).
- **Thermal** (`dev/powermng/bcmtemp`):
  - Attaches to the AVS monitor and reads its temperature register.
  - Uses the coefficients from `/thermal-zones/cpu-thermal`.
  - The result is `hw.sensors.bcmtemp0.temp0`.
- The x86 check of the MI changes (mii, brgphy, mmc) passed: quickkernel
  `X86_64_GENERIC` rebuilt all three with no warnings.

**Progress 6b (2026-10-06): remote management, the boot path and the
SD image** (fork 43e16b689c):
- **Remote management** (`run-netmgmt.sh`, 11/11):
  - The image boots with `ifconfig_vtnet0="DHCP"` and `sshd_enable` in
    `rc.conf`. Nothing is typed on the console.
  - The host logs in over the NIC (QEMU hostfwd) and checks the lease,
    the default route, `resolv.conf` and sshd on `*:22` (v4 and v6).
  - It then reboots over ssh. DHCP and sshd come back with the first
    boot's host key.
  - `vmexpect.py` gained a `host` step.
- **Console fallback** (`fdt_early.c`):
  - If `stdout-path` is missing or names something other than a PL011,
    the console is the first enabled `arm,pl011`.
  - The firmware's own `bcm2711-rpi-4-b.dtb` points `serial0` at the
    mini-UART. `disable-bt` moves it to the PL011 (`serial@7e201000`),
    so the fallback is a backstop.
- **Bootargs:**
  - The firmware prepends Linux words (`coherent_pool=`,
    `8250.nr_uarts=`, …) to `cmdline.txt`, so `boot_env` grew from 1 KB
    to 4 KB.
  - Words that still don't fit are counted and reported at boot instead
    of being dropped silently.
  - Checked by `run-console.sh` (10/10): the stdout-path is the PL061,
    and about 2 KB of filler words come before `vfs.root.mountfrom`.
- **SD image: `bin/arm-pisd`.**
  - MBR layout:
    - slice 1 is FAT32 `BOOT` (512 MB): firmware `PIFW_TAG` (cached in
      `tools/pifw`), `kernel8.img`, `config.txt`, `cmdline.txt` and
      `root-md.img`;
    - slice 2 is a bare UFS root (type 0x83, no disklabel; 0xA5 makes
      DragonFly log "cannot find label" on every open).
  - `config.txt`:
    - sets `kernel_address=0x200000`, `enable_uart`,
      `uart_2ndstage` and `dtoverlay=disable-bt`;
    - pulls in `include rootmode.txt`, which picks the md root (the
      firmware's `initramfs root-md.img followkernel`) or the SD root;
    - `README.txt` on the card says how to switch.
  - `rc.conf`: DHCP on `genet0`, sshd, dntpd and savetime.
  - SSH: the root key comes from `-k`, or a generated
    `WORK/id_ed25519`. Host keys are made on the host, so they stay the
    same across md boots.
  - The host has no mtools and the proxy blocks the Ubuntu archive, so
    `bin/fatput.py` fills a `mkfs.fat` image instead:
    - short names with NT lower-case flags, or VFAT long names with
      `~N` aliases;
    - contiguous files;
    - the `55 AA` trail on the sector after FSInfo, which BSD
      `fsck_msdosfs` wants.
  - `options MSDOSFS` added to `ARM64_RPI4` and `ARM64_VIRT`, for
    `/boot/firmware`.
- **`run-pisd.sh`** (33/33, then 13/13): QEMU boots the image (ARM64_VIRT
  kernel, `-D vbd0`) with root on `vbd0s2`. It checks:
  - `fsck_msdosfs`, mounting `/boot/firmware`, and the file names;
  - the checksums of `kernel8.img` and `root-md.img`;
  - `cmdline.txt`/`rootmode.txt`, the host key, `rc.conf` and sshd.
  - It then boots `root-md.img` as the md root.
- **Not testable here:** the Pi firmware's handling of the image:
  - `include`, `initramfs`, and the load address;
  - whether it reads our FAT.

**Progress 6c (2026-10-07): first boot on the board** (8 GB Pi 4B rev
1.5, `tools/pisd/pi-boot.log`). The md-root image boots multi-user to a
`login:` on the PL011.
- **Firmware:** it handled everything as planned. It reads our FAT,
  follows `include rootmode.txt`, loads the 300 MB initramfs at
  0x1c400000, applies `disable-bt` and puts `kernel8.img` at 0x200000.
  Reading `root-md.img` takes about 26 s.
- **Kernel start:** entered at EL2. 8052 MB of RAM, 4 CPUs by
  spin-table, GIC-400, and a 54 MHz timer.
- **Attached:** `bcmmbox`, `bcmwd`, `bcmrng`.
- **EMMC2:** `sdhci_bcm` → `mmcsd0`, a 29 GB SDHC card at 50 MHz,
  4-bit.
- **PCIe:** the link came up at 5 GT/s, and the VL805 shows up as
  `xhci0` on INTx (64-bit DMA). Its hubs enumerate.
- **Ethernet:** `genet0` + `brgphy0`, with the MAC from the DT.
- **rc:** dhclient, sshd, dntpd and cron start.
- **Not yet seen:** a DHCP lease (`genet0` said "no carrier" when
  dhclient started) and any umass disks (none were plugged in).
- **Bug:** the AVS monitor attached as `syscon0` instead of `bcmtemp`.
  DragonFly's `BUS_PROBE_*` are all 0, and the first 0 wins. Fixed in
  fork `954f47ac43`: `fdt_syscon` now probes at -100.
- **Harmless:** "simplebus0: cannot map interrupt 0..5" (twice) is the
  two HDMI nodes behind the `bcm2711-l2-intc`, which we don't drive.

**Progress 6d (2026-10-07): on the network.**
- **Network and management:** DHCP gave 192.168.25.194. ssh as root
  works with the `arm-pisd` key, and the host key matches
  `tools/pisd/hostkeys`.
- **Kernel update:** the newer kernel went onto the card over ssh
  (mount `/boot/firmware`; the old one is kept as `kernel8-old.img`).
  - DragonFly's msdosfs shows a renamed file in upper case
    (`KERNEL8.IMG`); the firmware doesn't care.
- **Temperature sensor:** with that kernel, `hw.sensors.bcmtemp0.temp0`
  reads about 48 °C (SoC).
- **USB hub:** a Sabrent HB-UMP3 shows up as a Genesys GL3510 pair. The
  USB3 half (`05e3:0626`) is at SuperSpeed on a VL805 root port. The USB2
  half (`05e3:0610`) hangs off the VL805's internal 2109 hub.
- **Bug: lost USB device nodes.** Toggling the hub's per-port power
  buttons re-attached it at the same address before the bus cleanup had
  destroyed the old cdev. devfs refused the new names, then deleted the
  old ones, so there was no `/dev/ugen0.3` and `usbconfig` couldn't see
  the hub. Fixed in fork `7c880b4be8`: `usb_destroy_dev()` now calls
  `destroy_dev()` at once; it is asynchronous and ordered on the devfs
  thread. Verified on the board: the hub dropped and re-attached at the
  same addresses, and all of `/dev/ugen0.1`–`0.6` survived.
- **Bug: clock never set at boot.** There's no RTC, so the clock started
  33 h behind. dntpd ran before dhclient wrote `resolv.conf`, and libc's
  resolver reads that file only once, so dntpd never resolved a server
  until it was restarted. Fixed in fork `483243dcd9`: `res_init()` after
  a failed `getaddrinfo()`.
  - Test: with `resolv.conf` hidden for the first 4 s, the new dntpd
    stepped the clock 33 h. On a reboot the quickset logs its COARSE
    adjustment.
  - The image's dntpd had predated `1bc64454c4` (no `-w`). It has been
    rebuilt shared, and `root-md.img` was pushed to the card.
- **x86 check:** these fixes are MI and still need `bin/arm-x86build`
  (`7c880b4be8`, `483243dcd9`, `3329cd2b82`, `7fb14d2f57`,
  `ee4cdd762a`).
- **SD root:** the firmware read the 300 MB `root-md.img` at about
  11.5 MB/s, which took 26 s. The card now boots the SD root
  (`rootmode-sd`/`cmdline-sd`).
  - Slice 2 was rewritten over ssh: `gzip -dc | dd of=/dev/mmcsd0s2`,
    with the sha256 checked afterwards.
  - From a reboot to ssh now takes 43 s, and `/` is `mmcsd0s2`.
- **Ethernet LEDs:** they showed link only. Fixed in fork `3329cd2b82`:
  brgphy now programs the BCM54213PE's LED1 and LED3 in multicolor
  mode, as the DT's `led-modes <0 8>` asks. On the board the green LED
  now blinks with traffic, and the amber one stays lit.
- **USB disks:** a JMicron JMS578 (`152d:0578`, fw 32.02) on the
  HB-UMP3 hub attaches as `da8` (238 GB, SuperSpeed).
  - **Bug: Sabrent JMS578 (`152d:a578`, fw 2.14) never got a disk.**
    umass attached, but every command timed out, both at boot and after
    a hot-plug. `usbconfig -d X.Y reset` fixed it by hand. It was not the
    drive: the same drive worked on the JMicron.
  - **Cause:** a `USB_DEBUG` kernel and a trace of each xHCI command
    showed it. The first Address Device (SET_ADDRESS) after power-on
    completes after about 630 ms with completion code 19, a context
    state error.
    - The slot stays in Default state at address 0. `usb_alloc_device()`
      ignored the error, which is fine when SET_ADDRESS is a plain
      control request. Control requests at address 0 still work, so the
      device attached, but its bulk endpoints could never be configured.
    - After that, the VL805 rejected every Address Device for the device
      at once. Port resets, Reset Device and a fresh slot (Disable Slot
      plus Enable Slot) didn't help, and neither did 90 s of retries.
    - It recovered only after a control request at address 0. The manual
      reset worked because umass's BBB resets had sent some by then.
  - **Fixed in fork `7fb14d2f57`:**
    - When a controller that sets the address itself fails, the stack
      now resets the port, reads 8 bytes of the device descriptor at
      address 0, and retries SET_ADDRESS once.
    - Address Device now gets 5 s, as on Linux, not 500 ms, so the slow
      failure completes instead of aborting the command ring.
    - Result: the Sabrent attaches at boot as `da9`, 238 GB.
  - **Ruled out:** the VL805 bulk-OUT burst quirk
    (`XHCI_VLI_SS_BULK_OUT_BUG`).
  - `sys/config/ARM64_RPI4_USBDEBUG` (fork, uncommitted) is ARM64_RPI4
    plus `options USB_DEBUG`, for the next USB problem.
- **Open:** `timeout(1)` on the Pi fails with "sigaction(32): Invalid
  argument".

**Progress 6e (2026-10-07): USB disk throughput.** Two 238 GB SSDs on
the HB-UMP3 (da8 JMicron, da9 Sabrent), bulk-only transport, raw `dd`.
The PCIe link is Gen2 x1, MPS 128.
- **Interrupt routing works:** ttyu0 on cpu 0, genet0 on 1, sdhci on 2,
  xhci0 on 3.
- **IMOD:** fork `ee4cdd762a` adds `dev.xhci.N.imod`. At 125 µs (500, the
  default) 4K reads at QD1 got 10.9 MB/s; at 40 µs (160, Linux's value)
  they got 24.9 MB/s. The default is unchanged for now.
- **Profile** (`options DEBUG_PCTRACK`, `kern.pctrack`) of a two-drive
  read at 240 MB/s: 37% memcpy (busdma bounce copies), 25% copyout
  (raw-device physio), 16% cache maintenance. The usbus0 thread used 57%
  of one cpu.
- **Bounces:** pci_brcmstb bounced all DMA above 960 MB, a limit taken
  from FreeBSD. The real limit is the outbound window at PCI
  0xc0000000: 3 GB, as in the DT's dma-ranges and on Linux.
  - Fork `a5a83a6113` makes 3 GB the default. Integrity check: 6 raw and
    12 UFS passes of 1 GB of random data read back intact.
  - On 8 GB, RAM above 3 GB still bounces.
  - Fork `557ab145b6` adds the `hw.physmem` tunable. At
    `hw.physmem=2g` (1893 MB available, like a 2 GB Pi), memcpy left the
    profile; copyout is 57%.
- **MAXPHYS:** fork `f34976c0cf` raises it to 1 MB on arm64, as Linux
  uses for SuperSpeed disks. iostat shows 1024 KB/t. On its own it
  barely changed throughput: the cost is per byte, not per command.

| raw dd, MB/s | before (960 MB, 128K) | 3 GB, 1M, 8 GB RAM | `hw.physmem=2g` |
|---|---|---|---|
| read 1M, one drive | 160 | 169 | 226 |
| read 64K | 126 | 140 | 174 |
| read 4K QD1 (imod 160) | 24.9 | — | 25.1 |
| read, both drives | 240 | 255 | 305 |
| write 1M, one drive | 149 | 145 | 176 |
| write, both drives | 218 | 210 | 248 |

- **Open:** whether to move the inbound window above 4 GB in PCI space
  so that all 8 GB is reachable (the VL805 does 64-bit DMA).
- Fork `7d0304c631` makes imod 160 (40 µs) the default.

**Progress 6f (2026-10-08): UAS.** A new `uas` driver (fork
`41a27691b7`) runs both JMS578 SSDs with tagged queueing over bulk
streams. Linux gives up on this combination; here it works with one
restriction. Both disks attach as `da0`/`da1` at `uas0`/`uas1` with
"Command Queueing Enabled". `hw.usb.uas.enable=0` falls back to umass.
- **xhci streams were broken** (fork `3483a53ae5`): stream context
  entries lacked SCT/DCS, Set TR Dequeue lacked SCT, and the event
  handler read the completion code as a stream ID. INQUIRY timed out.
- **The VL805 + JMS578 limit:** when reads longer than 8K overlap, the
  data-in stream stalls with a transfer-less event (xHCI §4.17.4).
  da0 (fw 32.02) fails at 16K QD2, da1 (fw 2.14) at 64K QD8. Reads of
  8K or less overlap fine at QD8; writes overlap at any size.
  - `hw.usb.uas.in_overlap` (default 8192): a longer read waits until no
    other read is in flight, and runs alone. -1 lifts the limit.
  - xhci recovers a stalled stream endpoint with a soft Reset Endpoint
    and escalates after 3 tries; `hw.usb.xhci.stream_resets` counts
    them. With the limit in place it stays at 0.
  - With `in_overlap=16384` the stall shows within a second; uas then
    resets the device by re-enumerating it, and the disk comes back as
    a **new da unit**. That is no good under a mounted filesystem or
    RAID, so in-place recovery (abort task, then a port reset without a
    detach) is still to do.
- **CAM fix** (fork `20bd09d601`): `xpt_bus_deregister()` dispatched
  queued CCBs without the send accounting, so a detaching SIM drove
  `send_active` negative.
- **Throughput** (`hw.physmem=2g`, random aligned I/O over 64 GB with
  `rr`, one drive unless noted):

| | BOT | UAS |
|---|---|---|
| 4K read QD1 | 25 | 18.6 (4550 IOPS) |
| 4K read QD8 | — | 48 (11.7k IOPS) |
| 8K read QD8 | — | 90 (11k IOPS) |
| 64K read QD1 / QD8 | 174 (seq.) | 146 / 167 |
| 1M read QD4 | 226 (seq.) | 299 |
| mixed 4K–1M QD8, both drives, 60 s | — | 95 each |
| write 64K–1M QD8, both drives | — | 126 each |

- **Integrity:** on both drives at once, 4 concurrent raw writes of
  768 MB of random data each, 4 concurrent read-backs, then 4 `cp`s
  onto fresh UFS and 4 read-backs after a remount: all 16 sha256 match,
  0 stream resets, no errors.
- **x86 check pending:** `7d0304c631`, `3483a53ae5` (xhci),
  `20bd09d601` (cam), `41a27691b7` and `0cf56b73f6` (umass, uas module).
- **In-place recovery** (fork `0cf56b73f6`, 2026-10-08). The test rig is
  now four Sabrent JMS578s (`152d:a578`, fw 2.14), da0–da3, on the
  Genesys hub.
  - **How it works:** a stall freezes the SIM queue and hands the
    outstanding commands back to CAM. The failed one spends a retry;
    the rest are requeued. A task then:
    1. resets the hub port (`usbd_req_re_enumerate`);
    2. restores SET_CONFIGURATION and the UAS alternate setting;
    3. marks the four endpoints halted, so xhci rebuilds their stream
       contexts on the next transfer.

    Then the queue is released and CAM replays the commands on the
    **same da unit**. Re-enumeration is now only the fallback when the
    reset fails.
  - **Locking:** `usbd_do_request()` takes the enumeration lock of the
    device it talks to, so the port reset needs the parent hub's lock.
    The hub thread holds the hub's lock while it takes the child's. The
    task therefore takes hub then device, polling (`LK_NOWAIT`), and
    gives up when detach (which holds both and drains the task) sets
    `sc_gone`. The first version took only the device's lock and
    deadlocked against hub explore.
  - **Per-device limit:** `dev.uas.N.in_overlap` starts at
    `hw.usb.uas.in_overlap`. It halves when a reset finds two or more
    reads moving data, and `dev.uas.N.resets` counts recoveries. Halving
    the *limit* matters: halving the largest read in flight gave
    2K–4K, because the hang is noticed late and its reads may be gone.
  - **Results:**
    - Forced storm (`in_overlap=-1`, 64K reads QD8 on all four disks
      while UFS was written and read back): 1968 in-place resets.
      All 12 files matched their sha256, with no I/O errors, no lost
      devices and no unit changes.
    - With learning, starting from 32K and mixed 4K–1M reads at QD8,
      every disk went 32K → 16K → 8K within 30 s (2–3 resets each).
      It then ran 60 s at about 50 MB/s each with no further resets.
      From -1 with 64K reads, a single reset to 32K, then 84 MB/s each.
    - Detach race: 18 recoveries plus 3 `usbconfig reset`s during a
      storm on da2. No panic; da2 came back as da2 each time, while
      da0 ran at 111 MB/s with 0 resets.
    - Regression at the defaults: integ2 16/16, 0 stream resets.
  - **Silent stalls:** above 8K, a stall sometimes raised no xhci event
    and was caught only by the CCB timeout (60 s). The default stays at
    8K, where neither kind of stall has been seen. A progress watchdog
    shorter than the CCB timeout would bound this; it is not done.
  - **Size, not total bytes.** Tested whether the bridge simply holds
    about 64K of read data, so that reads in flight may add up to 64K.
    Method: `in_overlap=-1`, fixed-size random reads, N threads, all
    four disks, 120 s per run. 8K × 7 (56K) ran clean at 26 MB/s each.
    Every overlap of reads over 8K stalled every disk:
    16K × 2 (32K total), 16K × 3/4/5/7, 32K × 2 (64K), 32K × 3, 64K × 2.
    The trigger is a read over 8K overlapping another read, however
    little data is in flight, so the per-read rule stays. One of the
    ~30 recoveries failed (`reset failed (USB_ERR_IOERROR)`), fell back
    to re-attach as designed, and its open descriptor got EINVAL.
  - **Cache flushes reach the SSDs.** Drives (ATA IDENTIFY through ATA
    PASS-THROUGH(16), which the bridges pass): da0/da1 Samsung 850 EVO
    250GB, da2/da3 Crucial BX500 240GB, all with the write cache on and
    FLUSH CACHE EXT. A libcam tool timed SCSI SYNCHRONIZE CACHE against
    ATA FLUSH CACHE EXT sent by pass-through, after bursts of random 4K
    writes. The two cost the same at every step: 0.25 ms idle (a TEST
    UNIT READY is 0.17), 0.65–2.9 ms after a single write. With the write
    cache turned off (SET FEATURES 0x82), writes took 4–9× longer and
    flushes fell back to idle cost. So the bridge turns SYNCHRONIZE CACHE
    into a real drive flush. `kern.cam.da.N.sync_cache` is 1 on all four.
    Whether the SSDs themselves honour it needs a power cut; neither
    model has power-loss protection.
- **Open:** a progress watchdog for silent stalls; the full product
  test at 2 GB.

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
- ~~Kernel modules on aarch64~~ (done 2026-10-06); installkernel still
  has to install them.
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
  - ~~Optional later: NEON GF(2^8) multiply for RAID6 Q parity~~ Done
    2026-10-06 (split-nibble `tbl`, `kernel_fpu_begin`); measure it on the
    Pi.

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
- ~~An `ddb` aarch64 disassembler~~ (done 2026-10-05) and ~~`minidump`
  support~~ (done 2026-10-05), plus `kgdb`.
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
6. **Tooling gaps:** no `kgdb` (ddb and its disassembler work since
   2026-10-05). QEMU's gdbstub is our
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
     the Phase 5 exit test passed (Progress 5e). The x86 check is
     done, and the rewritten RAID6 suite runs 123/0 (Progress 5f).
     `review-10-05.md` §6 items 1–3 are done and x86-checked
     (Progress 5f, 5g). Next: Phase 6 (Pi 4 bring-up). Item 4
     waits for Phase 9.
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
