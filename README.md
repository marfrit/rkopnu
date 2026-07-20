# rkopnu — RK Open NPU driver

An out-of-tree Linux kernel driver for the **Rockchip RK3588 NPU** that presents the
vendor **`rknpu` ioctl ABI** on top of a **mainline** kernel, so the closed vendor
userspace runtime **`librknnrt.so`** (and anything built on it, e.g. the RKNN matmul
backend of [rk-llama.cpp](https://github.com/invisiofficial/rk-llama.cpp)) runs on a
mainline kernel instead of the Rockchip BSP kernel.

It is a fork of the mainline `drivers/accel/rocket` driver with its uAPI swapped for the
vendor `rknpu` ABI. It reuses rocket's proven mainline plumbing (DT bind, GEM/shmem BOs,
IOMMU, `drm_sched`, threaded IRQ) and adds the register-command / job-submit programming
the vendor `rknpu` driver does, plus a permanent power-domain hold ("never-gate") and a
1 GHz OPP set at bind.

**Status:** functional and stability-tested. On an RK3588 (Rock 5 ITX) it reaches vendor
parity for a llama.cpp prefill workload — ~51.5 tok/s vs the vendor RKNPU2 stack's ~51.6
(gemma-4-E2B-it-Q8_0). A CoolPi CM5 GenBook runs a persistent `llama-server` at 128K
context through it, soak-tested at 150/150 completions with zero crashes/reboots, all
three NPU cores balanced. See "Performance notes" below.

> This driver runs the **closed** vendor `librknnrt.so`. It does not reimplement the NPU
> runtime; it provides the kernel ABI that runtime expects, on a mainline kernel.

---

## How it works

`librknnrt.so` talks to the kernel through a small set of `DRM_IOCTL_RKNPU_*` ioctls on a
DRM render node. rkopnu implements exactly those:

| ioctl | purpose |
|-------|---------|
| `RKNPU_ACTION` | probe: HW/driver version, IOMMU-enabled, frequency, SRAM sizes |
| `RKNPU_MEM_CREATE` | allocate a shmem-backed BO, map it into the NPU IOMMU domain |
| `RKNPU_MEM_MAP` | hand back the mmap fake-offset for a handle |
| `RKNPU_MEM_DESTROY` | drop a BO (IOMMU unmap on last ref) |
| `RKNPU_MEM_SYNC` | CPU↔device cache sync |
| `RKNPU_SUBMIT` | submit a register-command job to one or more of the 3 NPU cores, block until the IRQ completes it (the ABI is synchronous) |

`RKNPU_SUBMIT` honours the `core_mask` in each request — librknnrt round-robins
single-core matmuls across all three cores, so each job is pinned to the core it selected
and the three cores run in parallel.

---

## Requirements

**Hardware:** any RK3588 / RK3588S board.

**Kernel:** a mainline-based kernel that provides:
- `CONFIG_DRM_ACCEL=y` (the accel subsystem — the easiest way to get it is to enable the
  in-tree rocket driver, `CONFIG_DRM_ACCEL_ROCKET=m`, then blacklist it — see below),
- `CONFIG_DRM_SCHED`, `CONFIG_DRM_GEM_SHMEM_HELPER`, `CONFIG_ROCKCHIP_IOMMU`,
- the RK3588 `rknn_core@fdab0000/fdac0000/fdad0000` + `iommu@fdab9000/…` device-tree nodes
  (present in mainline `rk3588-base.dtsi`, **`status = "disabled"`** by default).

This was developed and tested against kernels carrying the mainline `accel/rocket` driver
and the RK3588 NPU DT bindings. The driver is self-contained — it does **not** depend on
any symbol from the in-tree rocket module; it only needs the kernel subsystems above.

**Userspace:** the vendor `librknnrt.so` (ships inside rk-llama.cpp at
`ggml/src/ggml-rknpu2/libs/`, or from the Rockchip `rknn-toolkit2` / `rknpu2` releases).

---

## 1. Enable the NPU device-tree nodes

Mainline ships the three RKNN cores + IOMMUs `status = "disabled"`. Enable them, attach an
OPP table for DVFS, and point the NPU regulator supplies at your board's NPU rail. An
example overlay (from a CoolPi CM5 GenBook) is in
[`dt/rk3588-npu-enable.dtso`](dt/rk3588-npu-enable.dtso).

**Board-specific:** the `npu-supply`/`sram-supply` phandles must point at *your* board's
NPU regulator (commonly `vdd_npu_s0`), and `&pd_npu`'s `domain-supply` must be wired (most
RK3588 board DTs already do this). The OPP microvolt values are the vendor V/f pairs.

Build and apply the overlay (extlinux `fdtoverlays`, U-Boot `fdt apply`, or fold the
`status = "okay"` changes straight into your board `.dts`). Verify after boot:

```
$ for n in fdab0000 fdac0000 fdad0000; do
    printf '%s ' $n; tr -d '\0' < /proc/device-tree/npu@$n/status; echo; done
fdab0000 okay
fdac0000 okay
fdad0000 okay
```

## 2. Blacklist the in-tree rocket driver

rkopnu registers its own `platform_driver` (name `"rkopnu"`) but matches the **same** DT
`compatible` as the in-tree `rocket` driver, so both would try to bind the NPU. Blacklist
the in-tree rocket so rkopnu wins the bind:

```
echo 'blacklist rocket' | sudo tee /etc/modprobe.d/rkopnu.conf
```

> The distinct driver name means `lsmod`, `/sys/bus/platform/drivers/rkopnu`, and `dmesg`
> (`rkopnu fdab0000.npu: …`) all agree — no collision with the in-tree rocket driver. The
> DRM render-node driver name stays `rknpu` (librknnrt identifies its device by that).

## 3. Build and load rkopnu

Out-of-tree build against your kernel's build tree:

```
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod ./rkopnu.ko          # or: install to updates/ + depmod for autoload
```

Verify it bound all three cores and created a render node:

```
$ dmesg | grep -i 'npu core'
rkopnu fdab0000.npu: Rockchip NPU core 0 version: 1179210309
rkopnu fdac0000.npu: Rockchip NPU core 1 version: 1179210309
rkopnu fdad0000.npu: Rockchip NPU core 2 version: 1179210309
$ ls /dev/dri/renderD*          # one of these is the NPU
```

## 4. Userspace: librknnrt + a client

Install the vendor runtime and raise the open-file limit — librknnrt exports a dmabuf fd
per handle and holds ~1500+ during a large prefill:

```
sudo install -m0644 librknnrt.so /usr/lib/librknnrt.so && sudo ldconfig
ulimit -n 65536
```

For LLM inference, build rk-llama.cpp with the RKNPU2 backend:

```
cmake -B build -G Ninja -DLLAMA_RKNPU2=ON -DGGML_ROCKET=OFF
ninja -C build llama-server
LD_LIBRARY_PATH=build/bin ./build/bin/llama-server -m model.gguf -c 131072 ...
```

Any request now runs its matmuls on the NPU through rkopnu. Confirm NPU activity via
rising interrupt counts for the three `npu` IRQs in `/proc/interrupts`.

---

## Performance notes

- Prefill reaches vendor-RKNPU2 parity because the same closed runtime does the compute;
  rkopnu's per-submit overhead (drm_sched worker + threaded IRQ) is ~3% of a submit — the
  NPU compute dominates. The one lever that mattered was honouring `core_mask` so all
  three cores run (an early version let drm_sched pick a core and left one idle).
- Board-to-board differences are **DDR-bandwidth**-bound, not clock: a GenBook (LPDDR5)
  runs ~49.5 tok/s vs a Rock 5 ITX (LPDDR5-6400) at ~51.5, both with the NPU at 1 GHz.
- **Multi-domain (>4GiB NPU-resident models), Rock 5 ITX, live-validated:** rkopnu honours
  the vendor `iommu_domain_id` (client-side `librknnrt`/RKNN-matmul callers, e.g.
  ggml-rknpu2's `IOMMUDomainManager`, already spread allocations across up to 16
  independent IOMMU paging domains — each domain gets its own ~4GiB IOVA aperture, the
  real ceiling coming from the vendor RKNN matmul API's int32 B-matrix offsets, not IOMMU
  hardware). pp512 prefill: Qwen2.5-3B-Instruct f16 (5.75 GiB, ≥2 domains) = 76.75 tok/s
  native (no INT8 fallback needed); Qwen3-30B-A3B-Instruct-2507 UD-Q4_K_XL (16.47 GiB,
  ~5 domains, MoE cross-domain expert routing) = 16.70 tok/s, exit 0, zero
  SError/panic/hung-task/VA-insert. A domain switch on one core's IOMMU submodule shares
  clock/PMU infrastructure with the cores' own power-domain resume (see
  `rocket_device_pm_get()`'s device-global `pm_lock`, used for exactly this class of
  cross-core race elsewhere in this driver); serializing IOMMU attach/detach against that
  same lock was what took the 30B case from a permanent D-state hang to a clean run.

## Limitations / notes

- Single-SoC RK3588 only. The register offsets and 3-core layout are RK3588-specific.
- rkopnu and the in-tree rocket driver match the same DT `compatible`, so rocket must be
  blacklisted for rkopnu to win the bind (see step 2).
- Decode (single-token generation) is not NPU-accelerated — that's a property of the
  runtime, not this driver.

## License

GPL-2.0 (kernel module; derived from `drivers/accel/rocket`, GPL-2.0). See
[LICENSE](LICENSE). `librknnrt.so` is Rockchip's proprietary runtime and is **not**
included or covered here.

## Provenance

Clean-room-on-mainline reconstruction of the vendor `rknpu` kernel ABI, cross-checked
against the vendor `drivers/rknpu` source for register/submit semantics, forked from
mainline `drivers/accel/rocket`.
