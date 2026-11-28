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

Test system: 2x GTX 1080 Ti (MSI factory-OC on x16 Gen3, Founders Edition
on x8 Gen3), i9-10940X, Windows VM on Proxmox KVM with both GPUs passed
through, NVIDIA 560.94. The display is on the MSI, which is also the
primary GPU (it composites and presents) and the card the single-GPU
baseline runs on. The log records which card is which (`MGPU: roles`).

**3DMark Fire Strike, graphics test 2, 3840x2160** (3DMark's own per-test
result; custom run, so no overall score):

| Configuration | GT2 fps |
|---|---|
| Single GPU, MSI | 20.58 |
| **Both GPUs, this fork** | **30.03** |

That is **+46% over the single card**. 3DMark itself reports one GPU and
SLI off: the application never knows a second GPU is rendering.

An earlier figure of +55-60% is withdrawn. It was measured with the display
on the other card, so its single-GPU baselines either ran on the slower
Founders Edition or had every frame copied across to the display's card.

**Where it does not help.** The gain depends on how much of a frame can be
split:

| Workload | Single | Dual | Why |
|---|---|---|---|
| BeamNG.drive, empty map, 3840x2160 | 45-48 fps | 44-46 fps | over half of each GPU's frame is full-screen compute, which both GPUs run whole |
| BeamNG.drive, West Coast USA, 1080p | 50.9 fps | 37.0 fps | the application thread is the limit; mirroring every command adds to it |

Both are measured with the fork's own instruments (PASSTIME and CALLTIME,
below), which is how the causes are known. These BeamNG runs predate the
`MGPU: roles` log line, so which card ran the single-GPU baseline was not
recorded; they are being re-measured on the setup above. Splitting compute by rows was
tried and is off by default: whether a dispatch may be split depends on
which rows its shader reads, which the bindings do not say.

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
- **PASSTIME:** GPU time per render target and per kind of compute
  dispatch, on each device, for one frame in 32. Shows which work is split
  and which both GPUs repeat.
- **CALLTIME:** application-thread time inside each D3D11 method, sampled.
  Found that copying each dynamic buffer to the second GPU on the
  application thread cost 5.2 ms per frame; that copy now runs on the
  worker's thread.
- **CGRAPH:** for one frame, which images each dispatch and render pass
  reads and writes.

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
git log --reverse d647fdf3..mgpu
```

## Known limitations

- Work that is not a screen-space draw (compute, shadow maps, off-screen
  passes, and all vertex work, since scissoring only splits fragments) runs
  in full on both GPUs. How much of a frame that is decides the gain: little
  in Fire Strike, over half in BeamNG.
- Every command is recorded twice on the CPU, so when the application thread
  is already the limit, two GPUs are slower than one.
- The composite costs ~1-2.5 ms per frame per GPU at 4K and adds one frame of
  latency.
- D3D11 only. Tested on NVIDIA only, two identical GPU models only.
- BeamNG shows a static diagonal line where shadows drop out in the lower
  right of the screen. Clean upstream DXVK at this fork's base does the same;
  native D3D11 does not.
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
| `DXVK_MGPU_PASSTIME=1` | per-pass GPU timing |
| `DXVK_MGPU_CALLTIME=1` | per-method CPU timing |
| `DXVK_MGPU_DUMP_GRAPH=1` | one frame's read/write graph |
| `DXVK_MGPU_COMPUTE_SPLIT=1` | experimental compute split (breaks BeamNG's post-processing) |

## License and credit

Same license as DXVK (zlib, see `LICENSE`). DXVK is the work of Philip
Rebohle and its contributors; upstream's README is kept as `README.dxvk.md`.

Multi-GPU work by Georgios Fragkoulis ([@ogiwrghs](https://github.com/ogiwrghs)).
