# Building for the Flashforge AD5M

How to cross-build `mjpg_streamer`, `input_uvc.so` and `output_http.so` for the AD5M with zmod, and how to install them on the printer.

## Target

- Allwinner R528, ARMv7, hard float, VFPv4-D16 (armhf).
- Kernel 5.4.61.
- zmod runs the streamer inside its chroot (`/data/.mod/.zmod`). The chroot has glibc 2.40 and `libjpeg.so.8`.
- zmod starts the binaries from `/usr/data/zmod/zmod/.shell/root/zcam/mjpg/` (see `/etc/init.d/S99camera`).

## Toolchain

This uses the Ubuntu 24.04 cross toolchain: GCC 13.3 and glibc 2.39, which is older than the printer's 2.40, so the binaries run there. The packages are extracted into a directory, here called `<xtools>`, with no system install needed.

- `<xtools>/root`: the compiler, extracted with `dpkg-deb -x <deb> <xtools>/root`:
  - `binutils-arm-linux-gnueabihf`
  - `cpp-13-arm-linux-gnueabihf`
  - `gcc-13-arm-linux-gnueabihf`
  - `gcc-13-arm-linux-gnueabihf-base`
  - `libc6-armhf-cross`
  - `libc6-dev-armhf-cross`
  - `libgcc-13-dev-armhf-cross`
  - `libgcc-s1-armhf-cross`
  - `libatomic1-armhf-cross`
  - `linux-libc-dev-armhf-cross`
- `<xtools>/sysroot2`: armhf libjpeg headers and libraries from `ports.ubuntu.com`. Extract `libjpeg-turbo8-dev` and `libjpeg-turbo8` (armhf) there. `input_uvc` links `libjpeg.so.8`.

The extracted binutils can't find their own libraries (`libbfd-2.42-armhf.so` and others). Run every `make` and every cross binutils command with:

```
export LD_LIBRARY_PATH=<xtools>/root/usr/lib/x86_64-linux-gnu
```

Without this, the build fails at the assembler with `error while loading shared libraries: libbfd-2.42-armhf.so`.

CMake toolchain file `<xtools>/armhf.cmake`:

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER <xtools>/root/usr/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_SYSROOT <xtools>/root)
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=vfpv4-d16 -mfloat-abi=hard")
set(CMAKE_FIND_ROOT_PATH <xtools>/root)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

## Configure and build

```
cd mjpg-streamer-experimental
mkdir _build-armhf-nov4l && cd _build-armhf-nov4l
X=<xtools>
cmake -DCMAKE_TOOLCHAIN_FILE=$X/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-march=armv7-a -mfpu=vfpv4-d16 -mfloat-abi=hard \
          -I$X/sysroot2/usr/include -I$X/sysroot2/usr/include/arm-linux-gnueabihf -U_TIME_BITS" \
      -DJPEG_LIB=$X/sysroot2/usr/lib/arm-linux-gnueabihf/libjpeg.so \
      -DCMAKE_SKIP_RPATH=ON -DV4L2_LIB=V4L2_LIB-NOTFOUND ..
LD_LIBRARY_PATH=$X/root/usr/lib/x86_64-linux-gnu make mjpg_streamer input_uvc output_http
```

- **`-U_TIME_BITS` is required.** Ubuntu's armhf compiler defaults to 64-bit `time_t`. That changes the size of `struct v4l2_buffer`, and with it the ioctl numbers of `VIDIOC_QUERYBUF`, `VIDIOC_QBUF` and `VIDIOC_DQBUF`. Kernel 5.4 has no time64 V4L2 ioctls and returns `ENOTTY`: "Unable to query buffer: Inappropriate ioctl for device".
- **`-DV4L2_LIB=V4L2_LIB-NOTFOUND`** builds without libv4l2. An MJPG camera doesn't need it, and it saves memory.
- **`-DCMAKE_SKIP_RPATH=ON`** keeps build-host paths out of the binaries.

The configuration in `_build-armhf` (with libv4l2 and RPATH) is not the one zmod ships. Don't take binaries from there.

## Strip and check

```
mkdir -p out && cp mjpg_streamer plugins/input_uvc/input_uvc.so plugins/output_http/output_http.so out/
LD_LIBRARY_PATH=$X/root/usr/lib/x86_64-linux-gnu $X/root/usr/bin/arm-linux-gnueabihf-strip out/*
readelf -V out/* | grep -o 'GLIBC_2\.[0-9]*' | sort -uV | tail -1   # at most GLIBC_2.38
readelf -d out/input_uvc.so | grep -E 'NEEDED|PATH'                 # libjpeg.so.8, libc, ld-linux; no RUNPATH
strings out/mjpg_streamer | grep 'git rev'
```

Use the host `readelf`, which reads ARM binaries fine. The extracted cross `readelf` needs `libctf-nobfd-armhf.so.0`, which is not in the extracted packages.

The build is reproducible. If a commit changes only `input_uvc`, then `mjpg_streamer` and `output_http.so` come out byte-identical to the previous build, and only `input_uvc.so` needs replacing. Compare with `md5sum`.

All three files must come from the same build when `struct _input` changes. Every plugin sees that struct, so in that case replace all three together.

## Install on the printer

Set `P` to your SSH login on the printer, e.g. `P=<user>@<printer address>`. The printer has no sftp server, so `scp` fails. Copy with `ssh … 'cat > file'`.

1. Check that the printer is not printing. The query should return `standby` or `complete`:
   ```
   ssh $P 'wget -qO- "http://127.0.0.1:7125/printer/objects/query?print_stats=state"'
   ```
2. Back up the current files outside the zmod checkout:
   ```
   ssh $P 'mkdir -p /data/.mod/.zmod/root/mjpg-backup && cp -a /usr/data/zmod/zmod/.shell/root/zcam/mjpg/* /data/.mod/.zmod/root/mjpg-backup/'
   ```
3. Upload to a temporary name, then rename. The running process keeps the old inode, so it is not overwritten under it:
   ```
   M=/usr/data/zmod/zmod/.shell/root/zcam/mjpg
   ssh $P "cat > $M/input_uvc.so.new" < out/input_uvc.so
   ssh $P "chmod 755 $M/input_uvc.so.new && mv $M/input_uvc.so.new $M/input_uvc.so && md5sum $M/*"
   ```
4. Restart the streamer. `zcam` restarts its child process 5 s after the child exits:
   ```
   ssh $P 'kill $(pidof mjpg_streamer)'
   ```
   Restarting the camera from the zmod interface also works.
5. Check the log `/opt/config/mod_data/log/cam/mjpg_streamer.log`. It should show `Estimated Frame Size: 614400` and no `bogus stream parameters`. A snapshot `http://<printer>:8080/?action=snapshot` should return a JPEG.

To roll back, copy the files from `/data/.mod/.zmod/root/mjpg-backup/` back the same way and restart the streamer.

A zmod update (`reset_git.sh`) overwrites the files with the version from the zmod repo.
