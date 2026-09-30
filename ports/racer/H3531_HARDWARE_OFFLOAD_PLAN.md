# Hi3531 Racer hardware-offload plan

Status: Stage 8.2 started the hardware path. The guiding rule is to use each
Hi3531 block for work it is designed to do while keeping a reversible software
fallback. No flash writes, module replacement, bootloader changes, or raw SoC
register writes are part of this plan.

## Current baseline

- CPU0: simulation + scene visibility + transforms + software triangle raster.
- CPU1: framebuffer presenter.
- HIFB: 1280x720 16-bit display surface.
- Internal render target: 640x360 ARGB1555.
- TDE, VOU/VO, VDEC, SYS/MMZ and HIFB kernel components are present on the
  target image; userspace vendor libraries are not required by the new TDE
  backend.

## Stage 8.2 — TDE full-frame present (implemented)

CPU still renders the 640x360 frame in normal cached memory. CPU1 copies that
small 450 KiB frame into an unused physical tail of HIFB memory. Hi3531 TDE
then performs the 640x360 -> 1280x720 ARGB1555 resize directly into the visible
framebuffer.

Why this is first:

- removes the old CPU 2x pixel expansion and ~3.5 MiB/frame framebuffer writes;
- uses a hardware block whose job is blit/resize/composite;
- needs no media-module reconfiguration;
- can fall back to the old CPU presenter immediately.

Runtime control:

- default: try TDE;
- RACER_TDE=0, RACER_TDE=off or RACER_TDE=cpu: force CPU exact2x;
- any TDE job failure: log it, disable TDE, continue through CPU exact2x.

Measure from the Racer log:

- present= : complete presenter time;
- copy= : cached frame -> HIFB TDE staging copy;
- job= / jobmax= : hardware TDE job time;
- fail= : number of hardware presentation failures.

## Stage 8.3 — cached MMZ render surfaces

Goal: remove the remaining 450 KiB staging copy.

Use two cached MMZ 640x360 ARGB1555 render buffers with physical addresses.
CPU renders directly into cached MMZ, flushes only the completed buffer, and
TDE consumes it as the source surface.

Requirements before enabling:

1. verify the exact Hi3531 MMZ allocation/remap/flush ioctl ABI;
2. preserve double buffering;
3. prove cache coherency with a deterministic test pattern;
4. retain the Stage 8.2 HIFB-tail and CPU presenters as fallbacks.

Expected result: CPU1 no longer copies pixels at all during normal present.

## Stage 8.4 — CPU1 scene preparation

Once TDE owns presentation, CPU1 should no longer spend its frame budget on
pixel scaling.

Move non-raster work for the next frame to CPU1:

- visible-sector scan;
- distance/frustum rejection;
- vertex world/camera transforms;
- triangle clipping and screen-space queue generation;
- depth-bin ordering.

CPU0 keeps simulation and the cache-sensitive textured raster pass. Use two
scene queues: CPU1 prepares N+1 while CPU0 rasterizes N. The frame snapshot
(camera/world transforms) must be immutable for the worker.

This is expected to matter more than TDE once city raster/queue load is high.

## Stage 8.5 — HIFB/VO composition

Investigate a separate hardware display/graphics layer for HUD and fixed UI.

Candidate split:

- layer A: TDE-presented 3D scene;
- layer B: HUD/minimap/text/icons with per-pixel/global alpha.

Only enable this after saving/restoring the existing HIFB state is proven.
The desktop/native lease must return exactly to its prior state on exit.

Benefits:

- HUD no longer dirties the 3D render target;
- static UI can update at a lower rate;
- TDE can clear/blit/composite sprites without triangle raster work.

## Stage 8.6 — media engines

Use media hardware only for media-shaped tasks:

- VDEC: H.264/H.265/MJPEG video decode where supported by the installed MPP;
- VO/VOU: decoded video presentation/composition;
- AO/ADEC: audio output/decode where appropriate;
- VPSS/VGS: video resize/format/post-processing where a video surface is the
  input/output.

Do not try to use VDEC/VPSS/VGS as a general 3D GPU.

Possible Racer uses:

- intro/cutscene playback;
- animated billboards/video screens;
- menu/background video;
- replay/video surfaces.

## IVE and other accelerators

IVE is useful for image-processing primitives, not arbitrary textured
triangles. Only use it if a concrete workload matches a supported primitive
(for example a future image-analysis or mask operation). Do not move physics
or the software 3D rasterizer there merely because the block exists.

## Performance decision rules

Each offload stage must provide an A/B mode and log:

- frame/render FPS;
- simulation Hz;
- presenter time;
- VC scan / queue / raster time;
- CPU wait/acquire/submit time;
- hardware job time and failures.

Keep an offload only when it reduces CPU wall time on the real Hi3531 board
without introducing tearing, stale-cache frames, input latency or lease/exit
regressions.

## Priority order

1. TDE full-frame resize: implemented in Stage 8.2.
2. MMZ cached render buffers: next hardware step.
3. CPU1 next-frame scene preparation: next CPU utilization step.
4. HIFB/VO HUD layer and TDE UI blits.
5. VDEC/VO/AO media paths when Racer gains media workloads.
6. Specialized accelerators only for workloads that actually match them.
