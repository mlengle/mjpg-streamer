/*
 * LD_PRELOAD stub of a UVC MJPEG camera at FAKE_DEV (default /dev/videofake).
 * Models uvcvideo on kernel 5.4: S_FMT and S_PARM each probe the camera and
 * overwrite the stored stream parameters; G_FMT and REQBUFS use them as is.
 *
 * FAKE_BAD=size|fps|both|slow (slow: 1 fps, below the advertised 5..30)
 *                        what the bogus probe answer spoils
 * FAKE_BAD_ON=sparm|sfmt  which probes can answer bogus (default sparm)
 * FAKE_BAD_FROM=N         first bogus probe, counting those probes from 1 (default 1)
 * FAKE_BAD_COUNT=K        that many bogus probes in a row (-1: all the rest)
 * FAKE_SIZE=N             the camera's honest max frame size (default W*H*2)
 *
 * Logs "FAKE: ..." lines to stderr for the test script.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define W 640
#define H 480
#define GOOD_SIZE (W * H * 2)
#define BAD_SIZE 0x3308d000u
#define BAD_FPS 45246u
#define MEM_SIZE (8 << 20)

static int fake_fd = -1;
static unsigned cur_size = 0, cur_num = 1, cur_den = 20;
static int sparm_calls, sfmt_calls, reqbufs_calls;
static const unsigned fps_list[] = { 30, 20, 15, 10, 5 };

static const char *dev(void)
{
    const char *d = getenv("FAKE_DEV");
    return d ? d : "/dev/videofake";
}

static int open_fake(void)
{
    fake_fd = memfd_create("fakecam", 0);
    if (fake_fd >= 0 && ftruncate(fake_fd, MEM_SIZE) < 0)
        return -1;
    fprintf(stderr, "FAKE: open\n");
    return fake_fd;
}

int open(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    va_list ap;
    mode_t mode;
    if (strcmp(path, dev()) == 0)
        return open_fake();
    if (!real)
        real = dlsym(RTLD_NEXT, "open");
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
    return real(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    va_list ap;
    mode_t mode;
    if (strcmp(path, dev()) == 0)
        return open_fake();
    if (!real)
        real = dlsym(RTLD_NEXT, "open64");
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
    return real(path, flags, mode);
}

static unsigned good_size(void)
{
    return getenv("FAKE_SIZE") ? (unsigned)atoi(getenv("FAKE_SIZE")) : GOOD_SIZE;
}

/* one probe of the camera, like uvc_probe_video(); n: this kind's call number */
static void probe(unsigned den, const char *kind, int n)
{
    const char *bad = getenv("FAKE_BAD");
    const char *on = getenv("FAKE_BAD_ON") ? getenv("FAKE_BAD_ON") : "sparm";
    int from = getenv("FAKE_BAD_FROM") ? atoi(getenv("FAKE_BAD_FROM")) : 1;
    int count = getenv("FAKE_BAD_COUNT") ? atoi(getenv("FAKE_BAD_COUNT")) : 0;
    int bogus = bad && !strcmp(on, kind) && n >= from && (count < 0 || n < from + count);

    cur_size = good_size();
    cur_num = 1;
    cur_den = den;
    if (bogus && (!strcmp(bad, "size") || !strcmp(bad, "both")))
        cur_size = BAD_SIZE;
    if (bogus && (!strcmp(bad, "fps") || !strcmp(bad, "both")))
        cur_den = BAD_FPS;
    if (bogus && !strcmp(bad, "slow"))
        cur_den = 1;
}

static void fill_fmt(struct v4l2_format *f)
{
    f->fmt.pix.width = W;
    f->fmt.pix.height = H;
    f->fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    f->fmt.pix.field = V4L2_FIELD_NONE;
    f->fmt.pix.bytesperline = 0;
    f->fmt.pix.sizeimage = cur_size;
}

static int fake_ioctl(unsigned long req, void *arg)
{
    switch (req) {
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *c = arg;
        memset(c, 0, sizeof(*c));
        strcpy((char *)c->driver, "uvcvideo");
        strcpy((char *)c->card, "fakecam");
        c->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        return 0;
    }
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *d = arg;
        if (d->index > 0)
            break;
        d->pixelformat = V4L2_PIX_FMT_MJPEG;
        strcpy((char *)d->description, "Motion-JPEG");
        return 0;
    }
    case VIDIOC_ENUM_FRAMESIZES: {
        struct v4l2_frmsizeenum *s = arg;
        if (s->index > 0 || s->pixel_format != V4L2_PIX_FMT_MJPEG)
            break;
        s->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        s->discrete.width = W;
        s->discrete.height = H;
        return 0;
    }
    case VIDIOC_ENUM_FRAMEINTERVALS: {
        struct v4l2_frmivalenum *i = arg;
        if (i->index >= sizeof(fps_list) / sizeof(fps_list[0]) ||
            i->pixel_format != V4L2_PIX_FMT_MJPEG || i->width != W || i->height != H)
            break;
        i->type = V4L2_FRMIVAL_TYPE_DISCRETE;
        i->discrete.numerator = 1;
        i->discrete.denominator = fps_list[i->index];
        return 0;
    }
    case VIDIOC_S_FMT:
        probe(cur_den == BAD_FPS || cur_den == 1 ? 20 : cur_den, "sfmt", ++sfmt_calls);
        fill_fmt(arg);
        fprintf(stderr, "FAKE: S_FMT sizeimage %u\n", cur_size);
        return 0;
    case VIDIOC_G_FMT:
        fill_fmt(arg);
        fprintf(stderr, "FAKE: G_FMT sizeimage %u\n", cur_size);
        return 0;
    case VIDIOC_G_PARM: {
        struct v4l2_streamparm *p = arg;
        memset(&p->parm, 0, sizeof(p->parm));
        p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        p->parm.capture.timeperframe.numerator = cur_num;
        p->parm.capture.timeperframe.denominator = cur_den;
        return 0;
    }
    case VIDIOC_S_PARM: {
        struct v4l2_streamparm *p = arg;
        unsigned want = p->parm.capture.timeperframe.denominator, best = fps_list[0], k;
        sparm_calls++;
        for (k = 0; k < sizeof(fps_list) / sizeof(fps_list[0]); k++)
            if (abs((int)fps_list[k] - (int)want) < abs((int)best - (int)want))
                best = fps_list[k];
        probe(best, "sparm", sparm_calls);
        p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
        p->parm.capture.timeperframe.numerator = cur_num;
        p->parm.capture.timeperframe.denominator = cur_den;
        fprintf(stderr, "FAKE: S_PARM #%d fps %u sizeimage %u\n", sparm_calls, cur_den, cur_size);
        return 0;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *rb = arg;
        reqbufs_calls++;
        fprintf(stderr, "FAKE: REQBUFS count %u size %u\n", rb->count, cur_size);
        if (rb->count && cur_size > MEM_SIZE / rb->count) {
            errno = ENOMEM;
            return -1;
        }
        return 0;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *b = arg;
        b->length = cur_size;
        b->m.offset = 0;
        return 0;
    }
    case VIDIOC_QBUF:
    case VIDIOC_STREAMON:
    case VIDIOC_STREAMOFF:
        return 0;
    case VIDIOC_DQBUF:
        sleep(10);
        errno = EAGAIN;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

int ioctl(int fd, unsigned long req, ...)
{
    static int (*real)(int, unsigned long, ...);
    va_list ap;
    void *arg;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (fd >= 0 && fd == fake_fd)
        return fake_ioctl((unsigned)req, arg);
    if (!real)
        real = dlsym(RTLD_NEXT, "ioctl");
    return real(fd, req, arg);
}

char *realpath(const char *path, char *resolved)
{
    static char *(*real)(const char *, char *);
    if (strcmp(path, dev()) == 0)
        return resolved ? strcpy(resolved, path) : strdup(path);
    if (!real)
        real = dlsym(RTLD_NEXT, "realpath");
    return real(path, resolved);
}

int close(int fd)
{
    static int (*real)(int);
    if (fd >= 0 && fd == fake_fd) {
        fprintf(stderr, "FAKE: close\n");
        fake_fd = -1;
    }
    if (!real)
        real = dlsym(RTLD_NEXT, "close");
    return real(fd);
}
