#!/bin/bash
# Startup tests of input_uvc against a fake UVC camera (fake_uvc.c, LD_PRELOAD).
# No camera needed; takes ~25 s and uses port 18431.
#
# Usage: run_tests.sh <build dir>
#   <build dir>: a host build of mjpg-streamer-experimental without libv4l2
#   (cmake -DV4L2_LIB=V4L2_LIB-NOTFOUND ..), so that ioctl() reaches the stub.
#   Set KEEP=1 to keep the logs.
[ -d "$1" ] || { echo "usage: $0 <build dir>" >&2; exit 2; }
B=$(cd "$1" && pwd)
SRC=$(cd "$(dirname "$0")" && pwd)
HERE=$(mktemp -d)
[ -n "$KEEP" ] && echo "logs: $HERE" || trap 'rm -rf "$HERE"' EXIT
gcc -Wall -shared -fPIC -o "$HERE/fake_uvc.so" "$SRC/fake_uvc.c" -ldl || exit 2
fail=0

run() {  # name, env...
    local name=$1; shift
    local t0=$(date +%s)
    local fpsarg="-f ${FPS:-20}"
    [ "$FPS" = none ] && fpsarg=""
    env "$@" LD_PRELOAD="$HERE/fake_uvc.so" LD_LIBRARY_PATH="$B" timeout ${TMO:-3} \
        "$B/mjpg_streamer" -i "$B/plugins/input_uvc/input_uvc.so -d /dev/videofake -r 640x480 $fpsarg -b 1" \
        -o "$B/plugins/output_http/output_http.so -p 18431" > "$HERE/$name.log" 2>&1
    echo $? > "$HERE/$name.rc"
    echo $(( $(date +%s) - t0 )) > "$HERE/$name.secs"
}
check() {  # name, description, condition
    if eval "$3"; then echo "PASS $1: $2"; else echo "FAIL $1: $2"; fail=1; fi
}
L() { cat "$HERE/$1.log"; }
rc() { cat "$HERE/$1.rc"; }
retries() { L "$1" | grep -c "bogus stream parameters"; }
big_reqbufs() { L "$1" | grep "FAKE: REQBUFS" | grep -vc "size 614400"; }
started() { L "$1" | grep -q "Estimated Frame Size: 614400" && [ "$(rc "$1")" = 124 ]; }

run clean
check clean "starts, no retry" 'started clean && [ $(retries clean) = 0 ]'
check clean "one S_PARM" '[ $(L clean | grep -c "FAKE: S_PARM") = 1 ]'

run size_once FAKE_BAD=size FAKE_BAD_COUNT=1
check size_once "starts after one retry" 'started size_once && [ $(retries size_once) = 1 ]'
check size_once "no REQBUFS with bogus size" '[ $(big_reqbufs size_once) = 0 ]'

run fps_once FAKE_BAD=fps FAKE_BAD_COUNT=1
check fps_once "starts after one retry" 'started fps_once && [ $(retries fps_once) = 1 ]'

run slow_once FAKE_BAD=slow FAKE_BAD_COUNT=1
check slow_once "fps below the advertised range: one retry" 'started slow_once && [ $(retries slow_once) = 1 ]'

FPS=1 run coerced
check coerced "legit coercion 1 -> 5 fps: no retry" 'started coerced && [ $(retries coerced) = 0 ] && L coerced | grep -q "from 1 to 5"'

FPS=none run nofps
check nofps "no -f: no S_PARM, starts, no retry" 'started nofps && [ $(retries nofps) = 0 ] && ! L nofps | grep -q "FAKE: S_PARM"'

FPS=none run nofps_bogus FAKE_BAD=fps FAKE_BAD_ON=sfmt FAKE_BAD_COUNT=1
check nofps_bogus "no -f, bogus fps from S_FMT: one retry" 'started nofps_bogus && [ $(retries nofps_bogus) = 1 ]'

run fixed_size FAKE_SIZE=2097152
check fixed_size "camera with a fixed 2 MB frame size: no retry" 'started fixed_size && [ $(retries fixed_size) = 0 ]'

TMO=10 run flaky_5s FAKE_BAD=size FAKE_BAD_COUNT=3
check flaky_5s "3 bogus answers in a row: starts on the 4th" 'started flaky_5s && [ $(retries flaky_5s) = 3 ]'

TMO=15 run always FAKE_BAD=both FAKE_BAD_COUNT=-1
check always "exits with error" '[ "$(rc always)" != 124 ] && [ "$(rc always)" != 0 ]'
check always "4 attempts" '[ $(L always | grep -c "FAKE: S_PARM") = 4 ]'
check always "backs off ~5 s in total" '[ $(cat $HERE/always.secs) -ge 4 ]'
check always "REQBUFS never called" '! L always | grep -q "FAKE: REQBUFS"'

exit $fail
