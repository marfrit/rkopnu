# rkopnu — RK Open NPU (mainline driver for librknnrt)

Goal: run the vendor `librknnrt.so` (efficient RK3588 NPU int8 GEMM, ~52 tok/s gemma-E2B-Q8)
on a MAINLINE kernel, by providing a clean mainline driver that speaks the `rknpu` ioctl ABI —
instead of optimizing ggml-rocket's GEMM ("fasterrocket", measured dead: 0.096 vs 1.83 TOPS,
19x below same-silicon ceiling; bigger batches tried 2x, no help).

## Strategy: fork mainline `rocket`, swap its uAPI for rknpu's
Reuse rocket's proven mainline plumbing (3-node `rockchip,rk3588-rknn-core` DT bind, GEM/shmem,
IOMMU, IRQ, register programming — `rocket_*.c` here) and replace its userspace-facing ioctl
layer with the 6 rknpu ioctls librknnrt calls, aggregating the 3 core devices into the vendor's
one-device multi-core (`core_mask`/`subcore_task[5]`) submit model.

## rknpu ABI to implement (measured live via strace, gemma-E2B prefill):
ACTION 0x40 (9807 calls, 26 sub-ops) · SUBMIT 0x41 (2028, multi-core core_mask) ·
MEM_CREATE 0x42 (3032) · MEM_MAP 0x43 (3032) · MEM_DESTROY 0x44 (3032) · MEM_SYNC 0x45 (14097)
+ DRM core PRIME_HANDLE_TO_FD/GEM_FLINK/VERSION. BOs are create+destroy churned (NOT persistent).
Device: librknnrt opens the /dev/dri/card node (not renderD). Spec: `rknpu_ioctl.h` (structs) +
vendor `rknpu_job.c` (register seq) + `rocket_job.c`/`rocket_registers.h` (cross-ref).

## Phases
0. [DONE] Scaffold: fork rocket, out-of-tree build → rkopnu.ko builds on 7.0.0-rc3-npuclk+.
1. Present rknpu uAPI: copy rknpu_ioctl.h as the driver's uapi, register a /dev/dri/card node,
   implement DRM_IOCTL_VERSION + RKNPU_ACTION(get hw/drv version) → librknnrt opens + probes OK.
2. Memory: RKNPU_MEM_CREATE/MAP/DESTROY/SYNC on rocket's GEM/shmem + IOMMU domains.
3. Submit+IRQ: RKNPU_SUBMIT → map rknpu task descriptors onto rocket's job/register machinery,
   multi-core via core_mask; IRQ/fence completion.
4. ACTION sub-ops (freq/power/iommu-domain/sram/reset) + integration: librknnrt end-to-end,
   target ~52 tok/s gemma-E2B on mainline.

## Test loop
rkopnu is mutually exclusive with rocket on the NPU (same DT nodes). Test: blacklist rocket at
boot, modprobe rkopnu, run librknnrt (rk-llama.cpp build/). Do NOT rmmod rocket live (SoC hang).
