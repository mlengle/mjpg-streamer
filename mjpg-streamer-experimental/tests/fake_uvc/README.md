# input_uvc startup tests with a fake camera

These tests check how `input_uvc` negotiates the format and frame rate at startup, and what it does when the camera answers with garbage. No camera is needed.

`fake_uvc.c` is an `LD_PRELOAD` library that pretends to be a UVC camera at `/dev/videofake`. `run_tests.sh` starts the real `mjpg_streamer` and `input_uvc.so` against it and checks their output.

## Requirements

- Linux with `gcc`, `bash` and `timeout` (coreutils).
- A **host** build of `mjpg-streamer-experimental` **without libv4l2**. With libv4l2, `input_uvc` calls `v4l2_ioctl()` from the library instead of `ioctl()`, and the stub is never reached.
- Port 18431 free (`output_http` listens there during the runs).

Build:

```
cd mjpg-streamer-experimental
mkdir -p _build && cd _build
cmake -DCMAKE_BUILD_TYPE=Release -DV4L2_LIB=V4L2_LIB-NOTFOUND ..
make mjpg_streamer input_uvc output_http
```

On Debian/Ubuntu you need `libjpeg-dev` for `input_uvc`.

## Run

From `mjpg-streamer-experimental`:

```
tests/fake_uvc/run_tests.sh _build
```

The script compiles the stub into a temporary directory. It prints one `PASS` or `FAIL` line per check and exits with 0 if all checks pass, 1 if any fails, and 2 on a usage or build error.

A run takes about 40 s, mostly the waits between retries. The script deletes the logs afterwards. Set `KEEP=1` to keep them; the script then prints their directory:

```
KEEP=1 tests/fake_uvc/run_tests.sh _build
```

Each scenario leaves `<name>.log` (the output of `mjpg_streamer` plus `FAKE: …` lines from the stub), `<name>.rc` (the exit code; 124 means it was still running when `timeout` stopped it, i.e. it started) and `<name>.secs`.

## What the stub models

- A 640×480 MJPEG camera that advertises 30, 20, 15, 10 and 5 fps.
- It behaves like `uvcvideo` on kernel 5.4:
  - `VIDIOC_S_FMT` and `VIDIOC_S_PARM` each "probe" the camera and overwrite the stored frame size and frame interval;
  - `VIDIOC_G_FMT`, `VIDIOC_G_PARM` and `VIDIOC_REQBUFS` return or use the stored values as they are.
- `REQBUFS` fails with `ENOMEM` if a buffer would exceed 8 MB, as `vmalloc` fails on the printer.
- `VIDIOC_DQBUF` never returns a frame. The tests cover only startup.

Environment variables choose which probes answer with garbage:

| Variable | Meaning |
|---|---|
| `FAKE_BAD=size\|fps\|both\|slow` | What the bogus answer spoils: frame size 0x3308d000 (856 MB), 45246 fps, both, or 1 fps (below the advertised range). |
| `FAKE_BAD_ON=sparm\|sfmt` | Which ioctl's probes may answer with garbage. Default: `sparm`. |
| `FAKE_BAD_FROM=N` | The first bogus probe, counting those probes from 1. Default: 1. |
| `FAKE_BAD_COUNT=K` | How many bogus probes in a row. `-1` means all the rest. Default: 0, i.e. none. |
| `FAKE_SIZE=N` | The camera's honest maximum frame size. Default: 614400 (W×H×2). |
| `FAKE_DEV=path` | The device path the stub serves. Default: `/dev/videofake`. Keep it under 16 characters, because `input_uvc` truncates longer names. |

## Scenarios

| Name | Setup | Expected |
|---|---|---|
| `clean` | normal camera, `-f 20` | starts, no retry, one `S_PARM` |
| `size_once` | first `S_PARM` gives an 856 MB frame size | one retry, starts, `REQBUFS` never sees the bogus size |
| `fps_once` | first `S_PARM` gives 45246 fps | one retry, starts |
| `slow_once` | first `S_PARM` gives 1 fps | one retry, starts |
| `coerced` | `-f 1`, camera minimum 5 fps | `FPS coerced from 1 to 5`, no retry |
| `nofps` | no `-f` | no `S_PARM`, starts, no retry |
| `nofps_bogus` | no `-f`, first `S_FMT` gives 45246 fps | one retry, starts |
| `fixed_size` | camera reports 2 MB at every resolution | no retry |
| `flaky_5s` | three bogus answers in a row | starts on the 4th attempt |
| `always` | every answer is bogus | 4 attempts, ~5 s of back-off, exits with an error, `REQBUFS` never called |

To check a scenario by hand, run `mjpg_streamer` with the stub preloaded:

```
gcc -shared -fPIC -o /tmp/fake_uvc.so tests/fake_uvc/fake_uvc.c -ldl
FAKE_BAD=size FAKE_BAD_COUNT=1 LD_PRELOAD=/tmp/fake_uvc.so LD_LIBRARY_PATH=_build \
  _build/mjpg_streamer -i "_build/plugins/input_uvc/input_uvc.so -d /dev/videofake -r 640x480 -f 20 -b 1" \
                       -o "_build/plugins/output_http/output_http.so -p 18431"
```

## Limitations

- The stub is not a real camera. On real hardware, check the result separately: a normal start on the printer, and pausing and resuming capture.
- Only MJPEG at 640×480 is modelled. The format and resolution fallbacks are not tested.
- The stub intercepts `open`, `open64`, `realpath`, `ioctl` and `close`. On x86_64 the ioctl number arrives sign-extended, because `xioctl()` passes it as an `int`. The stub compares only the lower 32 bits, as the kernel does.
