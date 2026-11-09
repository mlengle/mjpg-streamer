# Capture on demand (`input_uvc -idle`)

This fork adds capture on demand to `input_uvc`, plus fixes for the failure modes found along the way.

It is based on [DrA1ex/mjpg-streamer](https://github.com/DrA1ex/mjpg-streamer), which adds `-b` (buffer count) and `-fs` (frame size limit) to [jacksonliam/mjpg-streamer](https://github.com/jacksonliam/mjpg-streamer).

## Why

Upstream `input_uvc` captures frames all the time, whether anyone is watching or not. On a small board this has a cost even when the camera sits unused for hours:

- **USB traffic and interrupts.** The camera streams continuously over USB. On the AD5M the camera shares a USB 2.0 hub with the Wi-Fi adapter.
- **Kernel memory.** The vb2 capture buffers (`width × height × 2` per buffer, allocated with `vmalloc`) and the uvcvideo URB buffers stay allocated.

Only stopping the stream in the driver helps. Dropping frames in user space does not: ustreamer's `--slowdown` only dequeues less often, and measured the same USB interrupt rate as full-rate capture (see [Measurements](#measurements)).

## What changed

| Commit | Change |
|---|---|
| `919037a` | **Exit instead of hanging when the input thread gives up.** Before, clients waited forever for a frame and SIGINT hung, because `pthread_cond_destroy()` was called on a condvar that clients were still waiting on. Now the process exits with status 1, so a supervisor can restart it. |
| `06d50d5`, `ba750c6` | **Frame demand signalling.** `input_wait_frame()` in `plugins/input.h` stamps `in->demand` (`CLOCK_MONOTONIC`) and signals `in->demand_update` before waiting for a frame. Every output plugin uses it, so no output starves while capture is stopped. |
| `dff5fcd` | **`-idle N`.** When no frame has been requested for N seconds, `input_uvc` stops capture: `VIDIOC_STREAMOFF`, unmap, `REQBUFS 0`. That frees the vb2 and URB memory and stops the USB traffic. The first request after that restarts capture. `-idle 0`, the default, keeps the old behaviour. |
| `214526a`, `eadc77c`, `0c0fa25` | **Restart instead of exit on capture errors.** A failed DQBUF or a `select()` timeout restarts streaming. Restarts back off 1, 2, 4, then 8 s, and the backoff persists until a frame is published. After about a minute of failures, or immediately on ENODEV/ENXIO (camera unplugged), the thread gives up and the process exits (see `919037a`). If the driver grants fewer buffers than requested, that is now treated as a failure. |
| `ae40bd7` | **Drop the first frame after a restart.** On the AD5M camera the first frame after STREAMON was almost black in 4 of 10 wake-ups, because auto exposure had not settled yet. |

All plugins see `struct _input`, and the patch adds fields to it. So `mjpg_streamer` and every plugin it loads must come from the same build.

Not part of capture on demand, but in the same fork:

| Commit | Change |
|---|---|
| `06bf5db` | **Retry on bogus stream parameters.** Now and then the AD5M camera answers the startup negotiation with garbage, such as a 856 MB frame size or 45246 fps, and uvcvideo would allocate that frame size per buffer. Before `REQBUFS`, `input_uvc` now checks the frame size and the frame interval against limits and the camera's advertised intervals. It negotiates again after 0.5, 1.5 and 3 s, then gives up. |
| `924e624` | **Startup tests against a fake camera**, see [`tests/fake_uvc/README.md`](mjpg-streamer-experimental/tests/fake_uvc/README.md). |

## Usage

```
mjpg_streamer -o "output_http.so -p 8080" \
              -i "input_uvc.so -d /dev/video0 -r 640x480 -f 20 -b 1 -idle 30"
```

How to pick `N`:

- **Larger than the longest gap between requests from a client that should keep the camera warm.** Web UIs that poll `?action=snapshot` (Fluidd, Mainsail) request several frames per second while the page is visible, and stop when the tab is hidden.
- **Cold start latency.** The first frame after a pause takes about 0.6–0.7 s instead of 0.1 s.
- **Periodic snapshots.** A consumer that takes one snapshot less often than every N seconds, such as a per-layer timelapse, pays that latency on every frame.
- **Idle detection granularity.** It counts whole seconds and runs when a frame arrives, so capture stops N to N+2 s after the last request.

Each pause and wake-up writes one line to the log: `no frames requested for N s, stopping capture` and `restarting capture`.

Optional, not part of the patch: under snapshot polling, glibc keeps freed frame copies in per-thread arenas, and RSS grows. Setting these variables in the environment stops that:

```
MALLOC_ARENA_MAX=1 MALLOC_MMAP_THRESHOLD_=65536 mjpg_streamer ...
```

## Measurements

The test system:

- Flashforge AD5M: Allwinner R528 (2× Cortex-A7), 128 MB RAM, kernel 5.4.61;
- camera: USB UVC `ab20:1276`, MJPG only, 640×480 at 20 fps, `-b 1`;
- the printer was idle; snapshot polling ran on the printer itself with 3 clients, about 8 requests/s.

The fake V4L2 device used while developing capture on demand is not part of this repo. The fake camera in [`tests/fake_uvc`](mjpg-streamer-experimental/tests/fake_uvc/README.md) tests only startup negotiation, not pausing and resuming.

### Capture paused vs running

| | Capture running | `-idle 10`, paused |
|---|---|---|
| USB host controller interrupts (camera + Wi-Fi) | 317–336/s | **65/s** |
| vb2 buffer (`/proc/vmallocinfo`) | 1 × 604 KB | **freed** |

For comparison: ustreamer 6.42 with `--slowdown` and no clients measured 435/s, and stock mjpg-streamer 425/s, in the same session. Stopping the camera entirely gave 182/s; Wi-Fi activity differed between sessions.

### Wake-up

| | Before `ae40bd7` | After |
|---|---|---|
| First snapshot after a pause | 450–570 ms | 570–700 ms |
| Following snapshots | 80–110 ms | 80–110 ms |
| Dark first frame (~33 KB instead of ~98 KB) | **4 of 10** wake-ups | **0 of 10** |

Further checks, all passed:

- snapshot polling for 30 s: 0 false stops;
- 5 pause/wake-up cycles in a row: no memory growth, the vb2 buffer is freed each time;
- SIGTERM while paused: the process exits in 80 ms;
- dmesg: no uvcvideo or allocation errors.

### Memory, private dirty of the process

| Build | Idle | After 30 s of snapshot polling |
|---|---|---|
| stock (DrA1ex build, with libv4l2) | 508 KB | 824 KB |
| this fork, with libv4l2 | 512 KB | 828 KB |
| this fork, without libv4l2 | 444 KB (PSS −0.36 MB) | 760 KB |
| this fork + `MALLOC_ARENA_MAX=1 MALLOC_MMAP_THRESHOLD_=65536` | 504 KB | **528 KB** |

## Building for the AD5M

See [BUILD_AD5M.md](BUILD_AD5M.md): the cross toolchain, the CMake options (`-U_TIME_BITS` is required for kernel 5.4), checking the binaries, and installing them on the printer.
