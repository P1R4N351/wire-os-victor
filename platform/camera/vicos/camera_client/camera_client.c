/**
 * File: camera_client.h
 *
 * Author: Brian Chapados
 * Created: 01/24/2018
 *
 * Description:
 *               API for remote IPC connection to anki camera system daemon
 *
 * Copyright: Anki, Inc. 2018
 *
 **/

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <pthread.h>

#include "camera_client.h"

#define VIDEO_DEVICE    "/dev/video0"
#define SUBDEV_DEVICE   "/dev/v4l-subdev8"
#define CAMERA_SUBDEV_SCAN_MAX 32
#define CAMERA_MAX_CONSEC_EIO  8

#define CAPTURE_WIDTH_1MP   1280
#define CAPTURE_HEIGHT_1MP  720
#define CAPTURE_WIDTH_2MP   1600
#define CAPTURE_HEIGHT_2MP  1200
#define NUM_BUFFERS         4

#define EXPOSURE_LINES_PER_MS_1MP  22
#define EXPOSURE_LINES_PER_MS_2MP  37

#define EXPOSURE_MAX_LINES_1MP  1477
#define EXPOSURE_MAX_LINES_2MP  1199

#define GAIN_MIN_1MP   64
#define GAIN_MAX_1MP   255
#define GAIN_MIN_2MP   15
#define GAIN_MAX_2MP   79

#define NUM_RING_FRAMES 6

struct v4l2_buffer_info {
    void*  start;
    size_t length;
    int    queued;
};

struct frame_wrapper {
    anki_camera_frame_t* frame;
    size_t total_size;
};

struct camera_context {
    int video_fd;
    int subdev_fd;
    anki_camera_status_t status;

    int is_xray;

    struct v4l2_buffer_info buffers[NUM_BUFFERS];
    struct frame_wrapper wrappers[NUM_BUFFERS];
    uint32_t num_buffers;

    uint32_t frame_id;

    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    uint32_t frame_size;
    anki_camera_pixel_format_t pixel_format;

    pthread_t       capture_thread;
    pthread_mutex_t frame_mutex;
    pthread_cond_t  frame_cond;
    int             stop_pipe[2];

    anki_camera_frame_t* ring_frames[NUM_RING_FRAMES];
    anki_camera_frame_t* output_frame;
    size_t               frame_alloc_size;

    uint32_t ring_write_slot;
    uint32_t last_served_frame_id;
};

static struct camera_context g_ctx;

#define CAMERA_VFE_REOPEN_DELAY_MS 250
static struct timespec g_last_camera_close_time = {0, 0};

static int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static int v4l2_set_format(struct camera_context* ctx) {
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width  = ctx->is_xray ? CAPTURE_WIDTH_2MP  : CAPTURE_WIDTH_1MP;
    fmt.fmt.pix_mp.height = ctx->is_xray ? CAPTURE_HEIGHT_2MP : CAPTURE_HEIGHT_1MP;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SBGGR10P;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;

    if (xioctl(ctx->video_fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "camera: VIDIOC_S_FMT failed: %s\n", strerror(errno));
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(ctx->video_fd, VIDIOC_G_FMT, &fmt) < 0) {
            fprintf(stderr, "camera: VIDIOC_G_FMT failed: %s\n", strerror(errno));
            return -1;
        }
    }

    ctx->width = fmt.fmt.pix_mp.width;
    ctx->height = fmt.fmt.pix_mp.height;
    ctx->bytes_per_row = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    ctx->frame_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    ctx->pixel_format = ctx->is_xray ? ANKI_CAM_FORMAT_BAYER_MIPI_BGGR10_2MP
                                     : ANKI_CAM_FORMAT_BAYER_MIPI_BGGR10;
                                     
    return 0;
}

static int v4l2_request_buffers(struct camera_context* ctx)  {
    struct v4l2_requestbuffers req = {0};
    req.count = NUM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->video_fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "camera: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        return -1;
    }

    uint32_t target = req.count > NUM_BUFFERS ? NUM_BUFFERS : req.count;
    ctx->num_buffers = 0;

    for (uint32_t i = 0; i < target; i++) {
        struct v4l2_plane planes[1] = {{0}};
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (xioctl(ctx->video_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "camera: VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
            return -1;
        }

        ctx->buffers[i].length = planes[0].length;
        ctx->buffers[i].start = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, ctx->video_fd, planes[0].m.mem_offset);
        if (ctx->buffers[i].start == MAP_FAILED) {
            ctx->buffers[i].start = NULL;
            fprintf(stderr, "camera: mmap failed: %s\n", strerror(errno));
            return -1;
        }
        ctx->buffers[i].queued = 0;
        ctx->wrappers[i].total_size = sizeof(anki_camera_frame_t) + ctx->frame_size;
        ctx->wrappers[i].frame = malloc(ctx->wrappers[i].total_size);
        if (!ctx->wrappers[i].frame) {
            fprintf(stderr, "camera: wrapper allocation failed\n");
            return -1;
        }
        ctx->num_buffers = i + 1;
    }

    return 0;
}

static int v4l2_queue_all_buffers(struct camera_context* ctx) {
    for (uint32_t i = 0; i < ctx->num_buffers; i++) {
        struct v4l2_plane planes[1] = {{0}};
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (xioctl(ctx->video_fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "camera: VIDIOC_QBUF failed: %s\n", strerror(errno));
            return -1;
        }
        ctx->buffers[i].queued = 1;
    }
    return 0;
}

static int v4l2_start_streaming(struct camera_context* ctx) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->video_fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "camera: VIDIOC_STREAMON failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int v4l2_stop_streaming(struct camera_context* ctx) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->video_fd, VIDIOC_STREAMOFF, &type) < 0) {
        fprintf(stderr, "camera: VIDIOC_STREAMOFF failed: %s\n", strerror(errno));
        return -1;
    }

    for (uint32_t i = 0; i < ctx->num_buffers; i++) {
        ctx->buffers[i].queued = 0;
    }

    return 0;
}

static void v4l2_cleanup_buffers(struct camera_context* ctx)
{
    for (uint32_t i = 0; i < ctx->num_buffers; i++) {
        if (ctx->wrappers[i].frame) {
            free(ctx->wrappers[i].frame);
            ctx->wrappers[i].frame = NULL;
        }
    }

    for (uint32_t i = 0; i < ctx->num_buffers; i++) {
        if (ctx->buffers[i].start != NULL && ctx->buffers[i].start != MAP_FAILED) {
            munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            ctx->buffers[i].start = NULL;
        }
    }

    struct v4l2_requestbuffers req = {0};
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(ctx->video_fd, VIDIOC_REQBUFS, &req);

    ctx->num_buffers = 0;
}

static void resolve_video_device(char* out, size_t out_sz)
{
    const char* env_dev = getenv("ANKI_CAMERA_VIDEO_DEV");
    if (env_dev != NULL && env_dev[0] != '\0') {
        snprintf(out, out_sz, "%s", env_dev);
        return;
    }
    snprintf(out, out_sz, "%s", VIDEO_DEVICE);
}

static int match_sensor_name(const char* name)
{
    const char* custom = getenv("ANKI_CAMERA_MATCH_SUBDEV");
    if (custom != NULL && custom[0] != '\0') {
        return strncmp(name, custom, strlen(custom)) == 0;
    }
    return strncmp(name, "gc1066", 6) == 0 ||
           strncmp(name, "bf2253l", 7) == 0;
}

static void resolve_sensor_subdev(char* out, size_t out_sz)
{
    const char* env_subdev = getenv("ANKI_CAMERA_SUBDEV");
    if (env_subdev != NULL && env_subdev[0] != '\0') {
        snprintf(out, out_sz, "%s", env_subdev);
        return;
    }

    const char* sysfs_root = getenv("ANKI_CAMERA_SYSFS_ROOT");
    if (sysfs_root == NULL || sysfs_root[0] == '\0') {
        sysfs_root = "/sys/class/video4linux";
    }

    for (int i = 0; i < CAMERA_SUBDEV_SCAN_MAX; i++) {
        char path[160];
        char name[96];
        snprintf(path, sizeof(path), "%s/v4l-subdev%d/name", sysfs_root, i);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof(name), f) != NULL && match_sensor_name(name)) {
            fclose(f);
            snprintf(out, out_sz, "/dev/v4l-subdev%d", i);
            return;
        }
        fclose(f);
    }
    snprintf(out, out_sz, "%s", SUBDEV_DEVICE);
}

static void wait_vfe_reopen_cooldown(void)
{
    if (g_last_camera_close_time.tv_sec == 0) {
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    int64_t elapsed_ms = ((int64_t)now.tv_sec - (int64_t)g_last_camera_close_time.tv_sec) * 1000LL
                       + ((int64_t)now.tv_nsec - (int64_t)g_last_camera_close_time.tv_nsec) / 1000000LL;
    if (elapsed_ms < 0 || elapsed_ms >= CAMERA_VFE_REOPEN_DELAY_MS) {
        return;
    }
    int64_t rem_ms = CAMERA_VFE_REOPEN_DELAY_MS - elapsed_ms;
    struct timespec req = { .tv_sec = (time_t)(rem_ms / 1000LL),
                            .tv_nsec = (long)((rem_ms % 1000LL) * 1000000LL) };
    struct timespec rem = {0};
    for (int tries = 0; tries < 8; tries++) {
        if (nanosleep(&req, &rem) == 0 || errno != EINTR) {
            break;
        }
        req = rem;
    }
}

static void camera_init_fail_cleanup(void)
{
    if (g_ctx.num_buffers > 0) {
        v4l2_cleanup_buffers(&g_ctx);
    }
    if (g_ctx.video_fd >= 0) {
        close(g_ctx.video_fd);
        g_ctx.video_fd = -1;
    }
    if (g_ctx.subdev_fd >= 0) {
        close(g_ctx.subdev_fd);
        g_ctx.subdev_fd = -1;
    }
    pthread_cond_destroy(&g_ctx.frame_cond);
    pthread_mutex_destroy(&g_ctx.frame_mutex);
}

static void store_captured_frame(uint32_t buf_idx, uint64_t timestamp_ns)
{
    if (buf_idx >= g_ctx.num_buffers || g_ctx.buffers[buf_idx].start == NULL) {
        return;
    }
    pthread_mutex_lock(&g_ctx.frame_mutex);
    uint32_t slot = g_ctx.ring_write_slot % NUM_RING_FRAMES;
    g_ctx.ring_write_slot++;
    anki_camera_frame_t* f = g_ctx.ring_frames[slot];
    f->timestamp = timestamp_ns;
    f->frame_id = g_ctx.frame_id++;
    f->width = g_ctx.width;
    f->height = g_ctx.height;
    f->bytes_per_row = g_ctx.bytes_per_row;
    f->bits_per_pixel = 10;
    f->format = g_ctx.pixel_format;
    memcpy(f->data, g_ctx.buffers[buf_idx].start, g_ctx.frame_size);
    if (f->frame_id == 1 || f->frame_id % 150 == 0) {
        const uint8_t* d = (const uint8_t*)g_ctx.buffers[buf_idx].start;
        fprintf(stderr, "capture: frame %u slot %u ts=%llu data[0..3]=%02x %02x %02x %02x\n",
                f->frame_id, slot, (unsigned long long)timestamp_ns, d[0], d[1], d[2], d[3]);
    }
    pthread_cond_signal(&g_ctx.frame_cond);
    pthread_mutex_unlock(&g_ctx.frame_mutex);
}

static void* capture_thread_func(void* arg)
{
    (void)arg;
    int consec_eio = 0;

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_ctx.video_fd, &fds);
        FD_SET(g_ctx.stop_pipe[0], &fds);
        int nfds = (g_ctx.video_fd > g_ctx.stop_pipe[0] ? g_ctx.video_fd : g_ctx.stop_pipe[0]) + 1;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int r = select(nfds, &fds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "capture thread: select failed: %s\n", strerror(errno));
            break;
        }
        if (r == 0) continue;
        if (FD_ISSET(g_ctx.stop_pipe[0], &fds)) break;
        if (!FD_ISSET(g_ctx.video_fd, &fds)) continue;

        struct v4l2_plane planes[1] = {{0}};
        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                   .memory = V4L2_MEMORY_MMAP, .m.planes = planes, .length = 1 };
        if (xioctl(g_ctx.video_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            if (errno == ENODEV || ++consec_eio >= CAMERA_MAX_CONSEC_EIO) break;
            continue;
        }
        consec_eio = 0;
        if (buf.index < g_ctx.num_buffers && !(buf.flags & V4L2_BUF_FLAG_ERROR)) {
            uint64_t ts_ns = (uint64_t)buf.timestamp.tv_sec * 1000000000ULL
                           + (uint64_t)buf.timestamp.tv_usec * 1000ULL;
            store_captured_frame(buf.index, ts_ns);
        } else if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            fprintf(stderr, "capture thread: dropping corrupted buffer index=%u\n", buf.index);
        }
        buf.flags = 0;
        buf.m.planes = planes;
        if (xioctl(g_ctx.video_fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "capture thread: VIDIOC_QBUF failed: %s\n", strerror(errno));
        }
    }
    return NULL;
}

int camera_init(struct anki_camera_handle** camera, int is_xray)
{
    char video_dev[128];
    char subdev_dev[128];

    if (camera == NULL) return -1;
    wait_vfe_reopen_cooldown();

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.video_fd = -1;
    g_ctx.subdev_fd = -1;
    g_ctx.stop_pipe[0] = -1;
    g_ctx.stop_pipe[1] = -1;
    g_ctx.frame_id = 1;
    g_ctx.is_xray = is_xray;

    pthread_mutex_init(&g_ctx.frame_mutex, NULL);
    pthread_cond_init(&g_ctx.frame_cond, NULL);

    resolve_video_device(video_dev, sizeof(video_dev));
    resolve_sensor_subdev(subdev_dev, sizeof(subdev_dev));

    g_ctx.video_fd = open(video_dev, O_RDWR | O_NONBLOCK);
    if (g_ctx.video_fd < 0) {
        fprintf(stderr, "camera: Failed to open %s: %s\n", video_dev, strerror(errno));
        camera_init_fail_cleanup();
        return -1;
    }

    g_ctx.subdev_fd = open(subdev_dev, O_RDWR | O_NONBLOCK);
    if (v4l2_set_format(&g_ctx) < 0 || v4l2_request_buffers(&g_ctx) < 0) {
        camera_init_fail_cleanup();
        return -1;
    }

    g_ctx.status = ANKI_CAMERA_STATUS_IDLE;
    *camera = (struct anki_camera_handle*)&g_ctx;
    return 0;
}

static void stop_capture_thread_join(void)
{
    if (g_ctx.stop_pipe[1] >= 0) {
        char byte = 1;
        if (write(g_ctx.stop_pipe[1], &byte, 1) != 1) {
            fprintf(stderr, "stop_capture_thread: failed to write stop signal\n");
        }
        pthread_join(g_ctx.capture_thread, NULL);
        close(g_ctx.stop_pipe[0]);
        close(g_ctx.stop_pipe[1]);
        g_ctx.stop_pipe[0] = g_ctx.stop_pipe[1] = -1;
    }
}

static void stop_capture_thread(void)
{
    stop_capture_thread_join();

    for (int i = 0; i < NUM_RING_FRAMES; i++) {
        free(g_ctx.ring_frames[i]);
        g_ctx.ring_frames[i] = NULL;
    }
    free(g_ctx.output_frame);
    g_ctx.output_frame = NULL;
}

static int start_capture_thread_only(void)
{
    if (pipe(g_ctx.stop_pipe) < 0) {
        fprintf(stderr, "start_capture_thread: pipe failed: %s\n", strerror(errno));
        return -1;
    }
    if (pthread_create(&g_ctx.capture_thread, NULL, capture_thread_func, NULL) != 0) {
        fprintf(stderr, "start_capture_thread: pthread_create failed\n");
        close(g_ctx.stop_pipe[0]);
        close(g_ctx.stop_pipe[1]);
        g_ctx.stop_pipe[0] = g_ctx.stop_pipe[1] = -1;
        return -1;
    }
    return 0;
}

int camera_start(struct anki_camera_handle* camera) {
    usleep(200000);
    (void)camera;

    if (g_ctx.status == ANKI_CAMERA_STATUS_RUNNING) {
        return 0;
    }

    fprintf(stderr, "camera_start: starting camera (frame_id=%u)\n", g_ctx.frame_id);

    g_ctx.status = ANKI_CAMERA_STATUS_STARTING;

    if (v4l2_queue_all_buffers(&g_ctx) < 0) {
        g_ctx.status = ANKI_CAMERA_STATUS_IDLE;
        return -1;
    }

    if (v4l2_start_streaming(&g_ctx) < 0) {
        g_ctx.status = ANKI_CAMERA_STATUS_IDLE;
        return -1;
    }

    g_ctx.frame_alloc_size = sizeof(anki_camera_frame_t) + g_ctx.frame_size;
    for (int i = 0; i < NUM_RING_FRAMES; i++) {
        g_ctx.ring_frames[i] = malloc(g_ctx.frame_alloc_size);
        if (!g_ctx.ring_frames[i]) {
            fprintf(stderr, "camera_start: ring frame allocation failed at slot %d\n", i);
            for (int j = 0; j < i; j++) { free(g_ctx.ring_frames[j]); g_ctx.ring_frames[j] = NULL; }
            v4l2_stop_streaming(&g_ctx);
            g_ctx.status = ANKI_CAMERA_STATUS_IDLE;
            return -1;
        }
        memset(g_ctx.ring_frames[i], 0, g_ctx.frame_alloc_size);
    }
    g_ctx.output_frame = malloc(g_ctx.frame_alloc_size);
    if (!g_ctx.output_frame) {
        fprintf(stderr, "camera_start: output frame allocation failed\n");
        for (int i = 0; i < NUM_RING_FRAMES; i++) { free(g_ctx.ring_frames[i]); g_ctx.ring_frames[i] = NULL; }
        v4l2_stop_streaming(&g_ctx);
        g_ctx.status = ANKI_CAMERA_STATUS_IDLE;
        return -1;
    }
    memset(g_ctx.output_frame, 0, g_ctx.frame_alloc_size);
    g_ctx.ring_write_slot      = 0;
    g_ctx.last_served_frame_id = 0;

    if (start_capture_thread_only() != 0) {
        for (int i = 0; i < NUM_RING_FRAMES; i++) { free(g_ctx.ring_frames[i]); g_ctx.ring_frames[i] = NULL; }
        free(g_ctx.output_frame);
        g_ctx.output_frame = NULL;
        v4l2_stop_streaming(&g_ctx);
        g_ctx.status = ANKI_CAMERA_STATUS_IDLE;
        return -1;
    }

    g_ctx.status = ANKI_CAMERA_STATUS_RUNNING;
    fprintf(stderr, "camera_start: streaming started with background capture thread\n");
    return 0;
}

int camera_stop(struct anki_camera_handle* camera)
{
    (void)camera;

    if (g_ctx.status == ANKI_CAMERA_STATUS_OFFLINE) {
        return 0;
    }

    fprintf(stderr, "camera_stop: stopping camera (frame_id=%u)\n", g_ctx.frame_id);

    stop_capture_thread();

    v4l2_stop_streaming(&g_ctx);
    v4l2_cleanup_buffers(&g_ctx);

    if (g_ctx.video_fd >= 0) {
        close(g_ctx.video_fd);
        g_ctx.video_fd = -1;
        clock_gettime(CLOCK_MONOTONIC, &g_last_camera_close_time);
    }

    if (g_ctx.subdev_fd >= 0) {
        close(g_ctx.subdev_fd);
        g_ctx.subdev_fd = -1;
    }

    g_ctx.status = ANKI_CAMERA_STATUS_OFFLINE;

    return 0;
}

void camera_pause(struct anki_camera_handle* camera, int pause)
{
    (void)camera;

    if (pause) {
        if (g_ctx.status == ANKI_CAMERA_STATUS_RUNNING) {
            stop_capture_thread_join();
            v4l2_stop_streaming(&g_ctx);
            g_ctx.status = ANKI_CAMERA_STATUS_IDLE;
        }
    } else {
        if (g_ctx.status == ANKI_CAMERA_STATUS_IDLE && g_ctx.num_buffers > 0) {
            if (v4l2_queue_all_buffers(&g_ctx) < 0) {
                return;
            }
            if (v4l2_start_streaming(&g_ctx) < 0) {
                return;
            }
            if (start_capture_thread_only() != 0) {
                (void)v4l2_stop_streaming(&g_ctx);
                return;
            }
            g_ctx.status = ANKI_CAMERA_STATUS_RUNNING;
        }
    }
}

int camera_release(struct anki_camera_handle* camera) {
    (void)camera;

    if (g_ctx.status == ANKI_CAMERA_STATUS_RUNNING) {
        camera_stop(camera);
    }

    g_ctx.status = ANKI_CAMERA_STATUS_OFFLINE;
    return 0;
}

int camera_destroy(struct anki_camera_handle* camera)  {
    (void)camera;

    if (g_ctx.status != ANKI_CAMERA_STATUS_OFFLINE) {
        return 0;
    }

    v4l2_cleanup_buffers(&g_ctx);

    if (g_ctx.video_fd >= 0) {
        close(g_ctx.video_fd);
        g_ctx.video_fd = -1;
    }

    if (g_ctx.subdev_fd >= 0) {
        close(g_ctx.subdev_fd);
        g_ctx.subdev_fd = -1;
    }

    pthread_mutex_destroy(&g_ctx.frame_mutex);
    pthread_cond_destroy(&g_ctx.frame_cond);

    return 1;
}

int camera_frame_acquire(struct anki_camera_handle* camera, uint64_t frame_timestamp, anki_camera_frame_t** out_frame) {
    (void)camera;

    if (g_ctx.status != ANKI_CAMERA_STATUS_RUNNING) {
        return -1;
    }

    pthread_mutex_lock(&g_ctx.frame_mutex);

    anki_camera_frame_t* best = NULL;
    uint64_t best_ts = 0;

    for (int i = 0; i < NUM_RING_FRAMES; i++) {
        anki_camera_frame_t* f = g_ctx.ring_frames[i];
        if (f->timestamp == 0) continue;
        if (f->frame_id == g_ctx.last_served_frame_id) continue;
        if (frame_timestamp == 0 || f->timestamp <= frame_timestamp) {
            if (f->timestamp > best_ts) {
                best_ts = f->timestamp;
                best = f;
            }
        }
    }

    if (best == NULL) {
        pthread_mutex_unlock(&g_ctx.frame_mutex);
        return -1;
    }

    memcpy(g_ctx.output_frame, best, g_ctx.frame_alloc_size);
    g_ctx.last_served_frame_id = best->frame_id;

    static int first_serve_logged = 0;
    if (!first_serve_logged) {
        first_serve_logged = 1;
        const uint8_t* d = g_ctx.output_frame->data;
        fprintf(stderr, "camera_frame_acquire: first serve id=%u ts=%llu desired_ts=%llu "
                "fmt=%u %ux%u bpr=%u alloc=%zu "
                "data[0..9]=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                g_ctx.output_frame->frame_id,
                (unsigned long long)g_ctx.output_frame->timestamp,
                (unsigned long long)frame_timestamp,
                g_ctx.output_frame->format, g_ctx.output_frame->width,
                g_ctx.output_frame->height, g_ctx.output_frame->bytes_per_row,
                g_ctx.frame_alloc_size,
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9]);
    }

    pthread_mutex_unlock(&g_ctx.frame_mutex);

    *out_frame = g_ctx.output_frame;
    return 0;
}

int camera_frame_release(struct anki_camera_handle* camera, uint32_t frame_id) {
    (void)camera;
    (void)frame_id;

    return 0;
}

int camera_set_exposure(struct anki_camera_handle* camera, uint16_t exposure_ms, float gain) {
    (void)camera;

    if (g_ctx.subdev_fd < 0) {
        return -1;
    }

    struct v4l2_control ctrl = {0};
    ctrl.id = V4L2_CID_EXPOSURE;

    if (g_ctx.is_xray) {
        ctrl.value = (int)exposure_ms * EXPOSURE_LINES_PER_MS_2MP;
        if (ctrl.value > EXPOSURE_MAX_LINES_2MP) ctrl.value = EXPOSURE_MAX_LINES_2MP;
        if (ctrl.value < 1) ctrl.value = 1;
    } else {
        ctrl.value = (int)exposure_ms * EXPOSURE_LINES_PER_MS_1MP;
        if (ctrl.value > EXPOSURE_MAX_LINES_1MP) ctrl.value = EXPOSURE_MAX_LINES_1MP;
        if (ctrl.value < 1) ctrl.value = 1;
    }

    if (xioctl(g_ctx.subdev_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        fprintf(stderr, "camera: Failed to set exposure: %s\n", strerror(errno));
    }

    ctrl.id = V4L2_CID_GAIN;

    if (g_ctx.is_xray) {
        ctrl.value = (int)(gain * 11.15f + 3.85f);
        if (ctrl.value > GAIN_MAX_2MP) ctrl.value = GAIN_MAX_2MP;
        if (ctrl.value < GAIN_MIN_2MP) ctrl.value = GAIN_MIN_2MP;
    } else {
        ctrl.value = (int)(gain * 72.0f);
        if (ctrl.value > GAIN_MAX_1MP) ctrl.value = GAIN_MAX_1MP;
        if (ctrl.value < GAIN_MIN_1MP) ctrl.value = GAIN_MIN_1MP;
    }

    if (xioctl(g_ctx.subdev_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        fprintf(stderr, "camera: Failed to set gain: %s\n", strerror(errno));
    }

    return 0;
}

int camera_set_awb(struct anki_camera_handle* camera, float r_gain, float g_gain, float b_gain)
{
    (void)camera;
    (void)g_gain;

    if (g_ctx.subdev_fd < 0) {
        return -1;
    }

    // 0x01-0xFF
    int r_val = (int)(r_gain * 128.0f);
    int b_val = (int)(b_gain * 128.0f);

    if (r_val > 0xFF) r_val = 0xFF;
    if (r_val < 0x01) r_val = 0x01;
    if (b_val > 0xFF) b_val = 0xFF;
    if (b_val < 0x01) b_val = 0x01;

    struct v4l2_control ctrl = {0};

    ctrl.id = V4L2_CID_RED_BALANCE;
    ctrl.value = r_val;
    if (xioctl(g_ctx.subdev_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        // 2.0 doesn't implement this and frankly i cannot be bothered
        //fprintf(stderr, "camera: Failed to set R gain: %s\n", strerror(errno));
    }

    ctrl.id = V4L2_CID_BLUE_BALANCE;
    ctrl.value = b_val;
    if (xioctl(g_ctx.subdev_fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        //fprintf(stderr, "camera: Failed to set B gain: %s\n", strerror(errno));
    }

    return 0;
}

int camera_set_capture_format(struct anki_camera_handle* camera, anki_camera_pixel_format_t format)
{
    (void)camera;

    fprintf(stderr, "camera_set_capture_format: format=%d (0=BAYER,1=RGB,2=YUV,3=BAYER2MP,4=RGB2MP,5=YUV2MP)\n", (int)format);

    switch (format) {
        case ANKI_CAM_FORMAT_BAYER_MIPI_BGGR10:
        case ANKI_CAM_FORMAT_RGB888:
        case ANKI_CAM_FORMAT_YUV:
        case ANKI_CAM_FORMAT_BAYER_MIPI_BGGR10_2MP:
        case ANKI_CAM_FORMAT_RGB888_2MP:
        case ANKI_CAM_FORMAT_YUV_2MP:
            g_ctx.pixel_format = format;
            return 0;
        default:
            fprintf(stderr, "camera: unknown format %d\n", (int)format);
            return -1;
    }
}

int camera_set_capture_snapshot(struct anki_camera_handle* camera, uint8_t start)
{
    (void)camera;
    (void)start;

    return 0;
}

anki_camera_status_t camera_status(struct anki_camera_handle* camera)
{
    (void)camera;
    return g_ctx.status;
}
