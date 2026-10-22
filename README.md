# dxvk-mgpu - transparent split-frame multi-GPU rendering for D3D11

A fork of [DXVK](https://github.com/doitsujin/dxvk) that renders a single
Direct3D 11 application on **two GPUs at once**, without the application
knowing. The application issues one command stream; two independent Vulkan
devices each render part of the frame; the parts are composited before
Present.

No SLI bridge, no vendor multi-GPU API, no application support. Requests for
this in DXVK (issues #617, #639, 2018) were never implemented.

**Status:** experimental research project, one developer. Renders correctly
in the applications listed below on the hardware listed below. Not intended
for general use.

## Results

Test system: 2x GTX 1080 Ti (MSI factory-OC on x16 Gen3 as primary,
Founders Edition on x8 Gen3 as worker), i9-10940X, Windows VM on Proxmox KVM
with both GPUs passed through, NVIDIA 560.94.

| Workload | Single GPU | Dual GPU | Gain | Limit |
|---|---|---|---|---|
| 3DMark Fire Strike GT2, 3840x2160 | 23.7 fps | 29.5 fps | **+25%** | GPUs |
| 3DMark Fire Strike GT2, 1366x768 | 92.1 fps | 103.5 fps | +12% | application thread |
| BeamNG.drive, 3840x2160 | ~43 fps | ~47 fps | ~+9% | GPUs (to be re-measured) |
| BeamNG.drive, 1920x1200 | ~110 fps | ~110 fps | 0 | application thread |

Fire Strike figures are the mean frame rate over a full graphics-test run,
from the fork's own Present counters. Both GPUs are the same model, but the
worker card runs at lower clocks under load, so it gets the smaller share of
the frame.

**Renders correctly on both GPUs:** 3DMark Fire Strike (all graphics tests
and the physics test), BeamNG.drive, and the project's own D3D11 test
application across all of its feature axes.
**Not yet:** Unigine Superposition (not re-verified since several fixes);
GTA V Legacy (loads the system `d3d11.dll` instead of the local one).

## How it works

- **Two independent `VkDevice`s**, not a Vulkan device group, so the two
  GPUs need no bridge and no linked-adapter support.
- **Worker-side mirroring.** The primary GPU runs the normal DXVK context. A
  second `DxvkContext` on the worker GPU receives a mirrored copy of every
  command on its own CS thread. Every resource exists on both devices.
- **Split-frame rendering by scissor.** Screen-space passes (viewport within
  5% of the swapchain's aspect) are scissored: the primary renders the top
  part, the worker the bottom. Shadow maps, compute and other off-screen
  passes run in full on both GPUs.
- **Load balancing.** One global split line, adjusted every 30 frames from
  how long the primary waits for the worker. Each frame records the line it
  was rendered with, so a change never tears a frame.
- **Frame-behind composite.** At Present N the worker copies its rows of
  frame N into host-visible memory shared between the two devices
  (`VK_EXT_external_memory_host`), while the primary presents frame N-1
  with the worker's rows of N-1 pasted in. The CPU never waits for a full
  frame on either GPU; the cost is one frame of added latency.
- **Dynamic buffers.** DISCARD maps get their own slice on the worker, as
  on the primary. Buffers mapped with NO_OVERWRITE get a write-watch shadow
  (`VirtualAlloc(MEM_WRITE_WATCH)`), so only the dirty 4 KB pages cross to
  the second GPU. Deferred contexts and command lists are mirrored too.

## Debugging approach

Almost every bug in this project was silent: no error, no validation
message, just missing pixels on one half of the screen. The fork carries
its own instruments, all off the hot path or sampled:

- **RTDUMP / BUFDUMP:** with splitting disabled, both GPUs must produce
  byte-identical render targets and buffers. These copy every live target or
  buffer from both devices at the same point of each command stream and
  compare them byte for byte.
- **MGPUCTX:** a per-device census of all 246 `DxvkContext` methods. A
  command that reaches one device and not the other shows up in one run.
- **FRAMESPAN:** per-frame GPU timestamps on both devices on one time axis
  (`VK_EXT_calibrated_timestamps`), mapped onto the CPU clock, to see which
  GPU waits for which.

Examples of what they found: the hull shader's max tessellation factor was
pushed to the primary only, so the worker culled every tessellated patch;
shader unbinds were not mirrored, so the worker silently skipped 218,000
draws; DXVK hoisted buffer uploads ahead of the draws that used them; reading
write-combined memory ran at 12 MB/s; the VM's clock cost 3.5 us per read
until Hyper-V enlightenments were enabled.

**The commit history is the lab notebook.** Each commit message records
what was measured, what was ruled out, and which earlier conclusions were
retracted:

```
git log --reverse 02e2028c..mgpu
```

## Known limitations

- About half of a frame's GPU work (shadows, compute, off-screen passes, and
  all vertex work, since scissoring only splits fragments) is done in full on
  both GPUs. That caps the gain well below 2x.
- The composite costs ~1-2.5 ms per frame per GPU at 4K and adds one frame of
  latency.
- D3D11 only. Tested on NVIDIA only, two identical GPU models only.
- Some mirror paths are still immediate-context only or use staging copies.

## Building

MSYS2 MinGW64, Meson and Ninja, as for upstream DXVK:

```
meson setup --cross-file build-win64.txt build.w64
ninja -C build.w64
```

Copy `build.w64/src/d3d11/d3d11.dll` and `build.w64/src/dxgi/dxgi.dll` next to
the application's executable. Note: `build-win64.txt` still names
`x86_64-w64-mingw32-ar` and `windres`, which current MSYS2 ships as
`x86_64-w64-mingw32-gcc-ar` and under another name; a fresh setup needs those
two lines fixed.

### Environment variables

| Variable | Effect |
|---|---|
| `DXVK_MGPU_DISABLE=1` | single-GPU baseline |
| `DXVK_MGPU_FIXED_SPLIT=1` | split at 50%, no balancing |
| `DXVK_MGPU_NO_SCISSOR=1` | both GPUs render the whole frame (for byte compares) |
| `DXVK_MGPU_NO_PIPELINE=1` | synchronous composite instead of frame-behind |
| `DXVK_MGPU_DUMP_RT=WxH[@N]` | RTDUMP every N presents |
| `DXVK_MGPU_DUMP_BUF=N` | BUFDUMP every N presents |

## License and credit

Same license as DXVK (zlib, see `LICENSE`). DXVK is the work of Philip
Rebohle and its contributors; upstream's README is kept as `README.dxvk.md`.

Multi-GPU work by Georgios Fragkoulis ([@ogiwrghs](https://github.com/ogiwrghs)).
