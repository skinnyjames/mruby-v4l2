#ifndef MRUBY_V4L2
#define MRUBY_V4L2
#include "mruby_v4l2.h"
#include <string.h>
#include <errno.h>

void cleanup(mrb_v4l2_wrapper* camera)
{
    if (camera->fd == -1) return;

    if (camera->running) {
        camera->running = false;
        pthread_join(camera->thread, NULL);
        pthread_mutex_destroy(&camera->mutex);
        free(camera->front);
        free(camera->back);
        camera->front = NULL;
        camera->back  = NULL;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(camera->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < camera->buffer_count; i++) {
        if (camera->buffers[i].start && camera->buffers[i].start != MAP_FAILED)
            munmap(camera->buffers[i].start, camera->buffers[i].length);
    }
    close(camera->fd);
    camera->fd = -1;
}

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void uyvy_to_rgba_c(const uint8_t *uyvy, uint8_t *dst, int width, int height) {
    int num_pixels = width * height;
    for (int i = 0; i < num_pixels / 2; i++) {
        int uc = uyvy[i * 4 + 0] - 128;
        int y0 = uyvy[i * 4 + 1];
        int vc = uyvy[i * 4 + 2] - 128;
        int y1 = uyvy[i * 4 + 3];

        uint8_t *p0 = dst + (i * 2 + 0) * 4;
        uint8_t *p1 = dst + (i * 2 + 1) * 4;

        p0[0] = clamp_u8(y0 + ((359 * vc) >> 8));
        p0[1] = clamp_u8(y0 - ((88 * uc + 183 * vc) >> 8));
        p0[2] = clamp_u8(y0 + ((454 * uc) >> 8));
        p0[3] = 255;

        p1[0] = clamp_u8(y1 + ((359 * vc) >> 8));
        p1[1] = clamp_u8(y1 - ((88 * uc + 183 * vc) >> 8));
        p1[2] = clamp_u8(y1 + ((454 * uc) >> 8));
        p1[3] = 255;
    }
}

void bggr8_to_rgba_c(const uint8_t *bayer, uint8_t *dst, int width, int height)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            uint8_t *p = dst + idx * 4;

            int even_row = (y % 2 == 0);
            int even_col = (x % 2 == 0);

            uint8_t r, g, b;

            if (even_row && even_col) {
                b = bayer[idx];
                g = (x+1 < width  ? bayer[idx+1]       : b);
                r = (y+1 < height ? bayer[idx+width+1]  : b);
            } else if (even_row && !even_col) {
                g = bayer[idx];
                b = (x > 0        ? bayer[idx-1]        : g);
                r = (y+1 < height ? bayer[idx+width]    : g);
            } else if (!even_row && even_col) {
                g = bayer[idx];
                r = (x+1 < width  ? bayer[idx+1]        : g);
                b = (y > 0        ? bayer[idx-width]     : g);
            } else {
                r = bayer[idx];
                g = (x > 0        ? bayer[idx-1]        : r);
                b = (y > 0        ? bayer[idx-width-1]  : r);
            }

            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        }
    }
}

void yuyv_to_rgba_c(const uint8_t *yuyv, uint8_t *dst, int width, int height) {
    int num_pixels = width * height;

    for (int i = 0; i < num_pixels / 2; i++) {
        int y0 = yuyv[i * 4 + 0];
        int uc = yuyv[i * 4 + 1] - 128;
        int y1 = yuyv[i * 4 + 2];
        int vc = yuyv[i * 4 + 3] - 128;

        uint8_t *p0 = dst + (i * 2 + 0) * 4;
        uint8_t *p1 = dst + (i * 2 + 1) * 4;

        p0[0] = clamp_u8(y0 + ((359 * vc) >> 8));
        p0[1] = clamp_u8(y0 - ((88 * uc + 183 * vc) >> 8));
        p0[2] = clamp_u8(y0 + ((454 * uc) >> 8));
        p0[3] = 255;

        p1[0] = clamp_u8(y1 + ((359 * vc) >> 8));
        p1[1] = clamp_u8(y1 - ((88 * uc + 183 * vc) >> 8));
        p1[2] = clamp_u8(y1 + ((454 * uc) >> 8));
        p1[3] = 255;
    }
}

int setup_media_pipeline(
    int media_fd, int subdev_fd,
    int width, int height,
    const char* sensor_name,
    const char* other_sensor,
    const char* bridge_name,
    uint32_t bus_fmt_code)
{
    int sensor_entity_id = -1;
    int other_entity_id  = -1;
    int bridge_entity_id = -1;

    struct media_entity_desc entity = {0};
    for (int id = 0; ; id++) {
        entity.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) < 0) break;
        fprintf(stderr, "entity id=%d name='%s'\n", entity.id, entity.name);

        if (sensor_name  && strstr(entity.name, sensor_name))  sensor_entity_id = entity.id;
        if (other_sensor && strstr(entity.name, other_sensor)) other_entity_id  = entity.id;
        if (bridge_name && strstr(entity.name, bridge_name)) bridge_entity_id = entity.id;
    }

    if (sensor_entity_id < 0 || bridge_entity_id < 0) {
        fprintf(stderr, "couldn't find entities\n");
        return -1;
    }

    // Disable the other camera's link if present
    if (other_entity_id >= 0) {
        struct media_link_desc disable = {0};
        disable.source.entity = other_entity_id;
        disable.source.index  = 0;
        disable.source.flags  = MEDIA_PAD_FL_SOURCE;
        disable.sink.entity   = bridge_entity_id;
        disable.sink.index    = 0;
        disable.sink.flags    = MEDIA_PAD_FL_SINK;
        disable.flags         = 0;
        ioctl(media_fd, MEDIA_IOC_SETUP_LINK, &disable);
    }

    // Enable the selected camera's link
    struct media_link_desc link = {0};
    link.source.entity = sensor_entity_id;
    link.source.index  = 0;
    link.source.flags  = MEDIA_PAD_FL_SOURCE;
    link.sink.entity   = bridge_entity_id;
    link.sink.index    = 0;
    link.sink.flags    = MEDIA_PAD_FL_SINK;
    link.flags         = MEDIA_LNK_FL_ENABLED;

    if (ioctl(media_fd, MEDIA_IOC_SETUP_LINK, &link) < 0) {
        fprintf(stderr, "failed to enable link: %s\n", strerror(errno));
        return -1;
    }

    if (subdev_fd >= 0) {
        struct v4l2_subdev_format fmt = {0};
        fmt.which         = V4L2_SUBDEV_FORMAT_ACTIVE;
        fmt.pad           = 0;
        fmt.format.width  = width;
        fmt.format.height = height;
        fmt.format.code   = bus_fmt_code;
        fmt.format.field  = V4L2_FIELD_NONE;
        ioctl(subdev_fd, VIDIOC_SUBDEV_S_FMT, &fmt);
    }

    return 0;
}

void* camera_capture_thread(void* arg)
{
    mrb_v4l2_wrapper* cam = (mrb_v4l2_wrapper*)arg;

    while (cam->running) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }

        // If there's a newer frame queued, skip this one
        struct v4l2_buffer peek = {0};
        peek.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        peek.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam->fd, VIDIOC_DQBUF, &peek) == 0) {
            ioctl(cam->fd, VIDIOC_QBUF, &buf);
            buf = peek;
        }

        unsigned char* src = (unsigned char*)cam->buffers[buf.index].start;

        switch (cam->pixelformat) {
            case V4L2_PIX_FMT_YUYV:
                yuyv_to_rgba_c(src, cam->back, cam->width, cam->height);
                break;
            case V4L2_PIX_FMT_UYVY:
                uyvy_to_rgba_c(src, cam->back, cam->width, cam->height);
                break;
            case V4L2_PIX_FMT_SBGGR8:
                bggr8_to_rgba_c(src, cam->back, cam->width, cam->height);
                break;
            default:
                fprintf(stderr, "unsupported format: %.4s\n", (char*)&cam->pixelformat);
                ioctl(cam->fd, VIDIOC_QBUF, &buf);
                continue;
        }

        pthread_mutex_lock(&cam->mutex);
        unsigned char* tmp = cam->front;
        cam->front = cam->back;
        cam->back  = tmp;
        cam->ready = true;
        pthread_mutex_unlock(&cam->mutex);

        ioctl(cam->fd, VIDIOC_QBUF, &buf);
    }

    return NULL;
}

static void mrb_v4l2_type_free(mrb_state* mrb, void* payload)
{
    mrb_v4l2_wrapper* wrapper = (mrb_v4l2_wrapper*)payload;
    cleanup(wrapper);
    free(payload);
}

static struct mrb_data_type mrb_v4l2_type = { "Camera", mrb_v4l2_type_free };

mrb_v4l2_wrapper* mrb_v4l2_get(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* wrapper = (mrb_v4l2_wrapper*)DATA_PTR(self);
    if (!wrapper)
        mrb_raise(mrb, E_ARGUMENT_ERROR, "uninitialized camera data");
    return wrapper;
}

// Parse a string option from a Ruby hash by key name. Returns NULL if absent.
static const char* parse_str_opt(mrb_state* mrb, mrb_value options, const char* key)
{
    mrb_value v = mrb_hash_get(mrb, options, mrb_symbol_value(mrb_intern_cstr(mrb, key)));
    return mrb_nil_p(v) ? NULL : mrb_str_to_cstr(mrb, v);
}

// Parse a uint32 option from a Ruby hash by key name. Returns def if absent.
static uint32_t parse_uint_opt(mrb_state* mrb, mrb_value options, const char* key, uint32_t def)
{
    mrb_value v = mrb_hash_get(mrb, options, mrb_symbol_value(mrb_intern_cstr(mrb, key)));
    return mrb_nil_p(v) ? def : (uint32_t)mrb_int(mrb, v);
}

mrb_value mrb_v4l2_device_init(mrb_state* mrb, mrb_value self)
{
    char*    device;
    mrb_value rwidth, rheight;
    mrb_value options = mrb_nil_value();
    mrb_get_args(mrb, "zoo|H", &device, &rwidth, &rheight, &options);

    int width  = mrb_int(mrb, rwidth);
    int height = mrb_int(mrb, rheight);
    printf("%s - %d %d\n", device, width, height);

    // Config from Ruby — all optional, desktop works with no options
    const char* media_dev    = NULL;
    const char* subdev_dev   = NULL;
    const char* other_subdev = NULL;
    const char* sensor_name  = NULL;
    const char* other_sensor = NULL;
    const char* bridge_name  = NULL;
    uint32_t    bus_fmt_code = 0x3001;
    uint32_t    pixelformat  = V4L2_PIX_FMT_YUYV;

    if (!mrb_nil_p(options)) {
        media_dev    = parse_str_opt(mrb, options, "media_dev");
        subdev_dev   = parse_str_opt(mrb, options, "subdev_dev");
        other_subdev = parse_str_opt(mrb, options, "other_subdev_dev");
        sensor_name  = parse_str_opt(mrb, options, "sensor_name");
        other_sensor = parse_str_opt(mrb, options, "other_sensor_name");
        bridge_name  = parse_str_opt(mrb, options, "bridge_name");
        bus_fmt_code = parse_uint_opt(mrb, options, "bus_fmt_code", 0x3001);

        mrb_value rpfmt = mrb_hash_get(mrb, options,
                            mrb_symbol_value(mrb_intern_lit(mrb, "pixel_format")));
        if (!mrb_nil_p(rpfmt)) {
            const char* ps = mrb_sym2name(mrb, mrb_symbol(rpfmt));
            if      (strcmp(ps, "uyvy")   == 0) pixelformat = V4L2_PIX_FMT_UYVY;
            else if (strcmp(ps, "sbggr8") == 0) pixelformat = V4L2_PIX_FMT_SBGGR8;
            else                                pixelformat = V4L2_PIX_FMT_YUYV;
        }
    }

    int fd = open(device, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Can't open %s: %s (errno: %d)",
                 device, strerror(errno), errno);
        mrb_raise(mrb, E_STANDARD_ERROR, error_msg);
    }

    if (media_dev) {
        int media_fd = open(media_dev, O_RDWR);
        if (media_fd >= 0) {
            int subdev_fd = subdev_dev ? open(subdev_dev, O_RDWR) : -1;
            setup_media_pipeline(media_fd, subdev_fd,
                                 width, height,
                                 sensor_name, other_sensor,
                                 bridge_name, bus_fmt_code);
            if (subdev_fd >= 0) close(subdev_fd);
            close(media_fd);
        }
    }

    struct v4l2_format fmt = {0};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = pixelformat;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
        mrb_raise(mrb, E_STANDARD_ERROR, "can't apply ioctl settings to device");

    mrb_value obj = mrb_funcall(mrb, self, "new", 0, NULL);

    mrb_v4l2_wrapper* wrapper = malloc(sizeof(mrb_v4l2_wrapper));
    wrapper->fd           = fd;
    wrapper->width        = fmt.fmt.pix.width;
    wrapper->height       = fmt.fmt.pix.height;
    wrapper->pixelformat  = fmt.fmt.pix.pixelformat;
    wrapper->rgba_size    = wrapper->width * wrapper->height * 4;

    // Store config strings for later use by switch()
    // strdup memory leak?
    wrapper->media_dev    = media_dev    ? strdup(media_dev)    : NULL;
    wrapper->subdev_dev   = subdev_dev   ? strdup(subdev_dev)   : NULL;
    wrapper->other_subdev = other_subdev ? strdup(other_subdev) : NULL;
    wrapper->sensor_name  = sensor_name  ? strdup(sensor_name)  : NULL;
    wrapper->other_sensor = other_sensor ? strdup(other_sensor) : NULL;
    wrapper->bridge_name  = bridge_name  ? strdup(bridge_name)  : NULL;
    wrapper->bus_fmt_code = bus_fmt_code;

    mrb_data_init(obj, wrapper, &mrb_v4l2_type);
    return obj;
}

mrb_value mrb_v4l2_device_open(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);
    int fd = cam->fd;

    struct v4l2_requestbuffers req = {0};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
        mrb_raise(mrb, E_STANDARD_ERROR, "can't request buffer");
    cam->buffer_count = req.count;

    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
            mrb_raise(mrb, E_STANDARD_ERROR, "can't query video buffer");

        cam->buffers[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, buf.m.offset);
        cam->buffers[i].length = buf.length;

        if (cam->buffers[i].start == MAP_FAILED)
            mrb_raise(mrb, E_STANDARD_ERROR, "can't map buffer");

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
            mrb_raise(mrb, E_STANDARD_ERROR, "failed to queue buffer");
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
        mrb_raise(mrb, E_STANDARD_ERROR, "failed to start stream");

    cam->front   = malloc(cam->rgba_size);
    cam->back    = malloc(cam->rgba_size);
    cam->running = true;
    cam->ready   = false;
    pthread_mutex_init(&cam->mutex, NULL);
    pthread_create(&cam->thread, NULL, camera_capture_thread, cam);

    return mrb_true_value();
}

mrb_value mrb_v4l2_device_close(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);
    if (cam->fd == -1)
        mrb_raise(mrb, E_STANDARD_ERROR, "Camera already closed!");
    cleanup(cam);
    return mrb_nil_value();
}

mrb_value mrb_v4l2_device_req_frame(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);

    pthread_mutex_lock(&cam->mutex);
    if (!cam->ready) {
        pthread_mutex_unlock(&cam->mutex);
        return mrb_nil_value();
    }

    mrb_value rstr = mrb_str_new(mrb, (const char*)cam->front, cam->rgba_size);
    cam->ready = false;
    pthread_mutex_unlock(&cam->mutex);

    return rstr;
}

// Switch to a new camera config passed as a Ruby options hash.
// The hash has the same shape as the one passed to init —
// Ruby owns the decision of which config to switch to (e.g. V4L2::PINEPHONE_FRONT).
mrb_value mrb_v4l2_device_switch_camera(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);
    mrb_value options;
    mrb_get_args(mrb, "H", &options);

    const char* media_dev    = parse_str_opt(mrb, options, "media_dev");
    const char* subdev_dev   = parse_str_opt(mrb, options, "subdev_dev");
    const char* other_subdev = parse_str_opt(mrb, options, "other_subdev_dev");
    const char* sensor_name  = parse_str_opt(mrb, options, "sensor_name");
    const char* other_sensor = parse_str_opt(mrb, options, "other_sensor_name");
    const char* bridge_name  = parse_str_opt(mrb, options, "bridge_name");
    uint32_t    bus_fmt_code = parse_uint_opt(mrb, options, "bus_fmt_code", cam->bus_fmt_code);

    if (!media_dev) {
        fprintf(stderr, "no media_dev in config, switch is a no-op\n");
        return mrb_false_value();
    }

    // Stop capture thread
    cam->running = false;
    pthread_join(cam->thread, NULL);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);

    int media_fd = open(media_dev, O_RDWR);
    if (media_fd >= 0) {
        int subdev_fd = subdev_dev ? open(subdev_dev, O_RDWR) : -1;
        setup_media_pipeline(media_fd, subdev_fd,
                             cam->width, cam->height,
                             sensor_name, other_sensor,
                             bridge_name, bus_fmt_code);
        if (subdev_fd >= 0) close(subdev_fd);
        close(media_fd);
    }

    // Update stored config
    free(cam->media_dev);    cam->media_dev    = media_dev    ? strdup(media_dev)    : NULL;
    free(cam->subdev_dev);   cam->subdev_dev   = subdev_dev   ? strdup(subdev_dev)   : NULL;
    free(cam->other_subdev); cam->other_subdev = other_subdev ? strdup(other_subdev) : NULL;
    free(cam->sensor_name);  cam->sensor_name  = sensor_name  ? strdup(sensor_name)  : NULL;
    free(cam->other_sensor); cam->other_sensor = other_sensor ? strdup(other_sensor) : NULL;
    free(cam->bridge_name);  cam->bridge_name  = bridge_name  ? strdup(bridge_name)  : NULL;
    cam->bus_fmt_code = bus_fmt_code;

    // Handle pixel format change if specified
    mrb_value rpfmt = mrb_hash_get(mrb, options,
                        mrb_symbol_value(mrb_intern_lit(mrb, "pixel_format")));
    if (!mrb_nil_p(rpfmt)) {
        const char* ps = mrb_sym2name(mrb, mrb_symbol(rpfmt));
        if      (strcmp(ps, "uyvy")   == 0) cam->pixelformat = V4L2_PIX_FMT_UYVY;
        else if (strcmp(ps, "sbggr8") == 0) cam->pixelformat = V4L2_PIX_FMT_SBGGR8;
        else                                cam->pixelformat = V4L2_PIX_FMT_YUYV;
    }

    ioctl(cam->fd, VIDIOC_STREAMON, &type);
    cam->running = true;
    cam->ready   = false;
    pthread_create(&cam->thread, NULL, camera_capture_thread, cam);

    return mrb_true_value();
}

mrb_value mrb_v4l2_device_width(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* wrapper = mrb_v4l2_get(mrb, self);
    return mrb_int_value(mrb, wrapper->width);
}

mrb_value mrb_v4l2_device_height(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* wrapper = mrb_v4l2_get(mrb, self);
    return mrb_int_value(mrb, wrapper->height);
}

void mrb_mruby_v4l2_gem_init(mrb_state* mrb)
{
    struct RClass* mod   = mrb_define_module(mrb, "V4L2");
    struct RClass* klass = mrb_define_class_under(mrb, mod, "Camera", mrb->object_class);
    MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
    mrb_define_class_method(mrb, klass, "init",   mrb_v4l2_device_init,          MRB_ARGS_REQ(3) | MRB_ARGS_OPT(1));
    mrb_define_method(mrb, klass, "open",         mrb_v4l2_device_open,          MRB_ARGS_NONE());
    mrb_define_method(mrb, klass, "close",        mrb_v4l2_device_close,         MRB_ARGS_NONE());
    mrb_define_method(mrb, klass, "frame",        mrb_v4l2_device_req_frame,     MRB_ARGS_NONE());
    mrb_define_method(mrb, klass, "width",        mrb_v4l2_device_width,         MRB_ARGS_NONE());
    mrb_define_method(mrb, klass, "height",       mrb_v4l2_device_height,        MRB_ARGS_NONE());
    mrb_define_method(mrb, klass, "switch",       mrb_v4l2_device_switch_camera, MRB_ARGS_REQ(1));
}

void mrb_mruby_v4l2_gem_final(mrb_state* mrb)
{
}

#endif