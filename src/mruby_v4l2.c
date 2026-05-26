
#ifndef MRUBY_V4L2
#define MRUBY_V4L2
#include "mruby_v4l2.h"
#include <string.h>
#include <errno.h>

void cleanup(mrb_v4l2_wrapper* camera)
{
  if (camera->fd == -1) return;

  // Stop thread if running
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

            // BGGR pattern:
            // B G B G
            // G R G R
            int even_row = (y % 2 == 0);
            int even_col = (x % 2 == 0);

            uint8_t r, g, b;

            if (even_row && even_col) {
                // B pixel
                b = bayer[idx];
                g = (x+1 < width ? bayer[idx+1] : b);
                r = (y+1 < height ? bayer[idx+width+1] : b);
            } else if (even_row && !even_col) {
                // G pixel (blue row)
                g = bayer[idx];
                b = (x > 0 ? bayer[idx-1] : g);
                r = (y+1 < height ? bayer[idx+width] : g);
            } else if (!even_row && even_col) {
                // G pixel (red row)
                g = bayer[idx];
                r = (x+1 < width ? bayer[idx+1] : g);
                b = (y > 0 ? bayer[idx-width] : g);
            } else {
                // R pixel
                r = bayer[idx];
                g = (x > 0 ? bayer[idx-1] : r);
                b = (y > 0 ? bayer[idx-width-1] : r);
            }

            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        }
    }
}
// dst must point to a buffer of at least (width * height * 4) bytes
void yuyv_to_rgba_c(const uint8_t *yuyv, uint8_t *dst, int width, int height) {
    int num_pixels = width * height;

    for (int i = 0; i < num_pixels / 2; i++) {
        int y0 = yuyv[i * 4 + 0];
        int uc = yuyv[i * 4 + 1] - 128;
        int y1 = yuyv[i * 4 + 2];
        int vc = yuyv[i * 4 + 3] - 128;

        uint8_t *p0 = dst + (i * 2 + 0) * 4;
        uint8_t *p1 = dst + (i * 2 + 1) * 4;

        // Pixel 0
        p0[0] = clamp_u8(y0 + ((359 * vc) >> 8));   // R
        p0[1] = clamp_u8(y0 - ((88 * uc + 183 * vc) >> 8)); // G
        p0[2] = clamp_u8(y0 + ((454 * uc) >> 8));   // B
        p0[3] = 255;                                  // A

        // Pixel 1 (shared chroma)
        p1[0] = clamp_u8(y1 + ((359 * vc) >> 8));
        p1[1] = clamp_u8(y1 - ((88 * uc + 183 * vc) >> 8));
        p1[2] = clamp_u8(y1 + ((454 * uc) >> 8));
        p1[3] = 255;
    }
}

int setup_media_pipeline(int media_fd, int subdev_fd, int width, int height, camera_facing facing)
{
    int sensor_entity_id  = -1;
    int front_entity_id   = -1;
    int rear_entity_id    = -1;
    int bridge_entity_id  = -1;

    struct media_entity_desc entity = {0};
    for (int id = 1; ; id++) {
        entity.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) < 0) break;

        if (strstr(entity.name, "ov5640"))
            rear_entity_id = entity.id;
        if (strstr(entity.name, "gc2145"))
            front_entity_id = entity.id;
        if (strstr(entity.name, "sun6i-csi-bridge") ||
            strstr(entity.name, "sun6i-csi"))
            bridge_entity_id = entity.id;
    }

    sensor_entity_id = (facing == CAMERA_FRONT) ? front_entity_id : rear_entity_id;

    if (sensor_entity_id < 0 || bridge_entity_id < 0) {
        fprintf(stderr, "couldn't find entities\n");
        return -1;
    }

    // Disable the other camera's link first
    int other_entity_id = (facing == CAMERA_FRONT) ? rear_entity_id : front_entity_id;
    if (other_entity_id >= 0) {
        struct media_link_desc disable = {0};
        disable.source.entity = other_entity_id;
        disable.source.index  = 0;
        disable.source.flags  = MEDIA_PAD_FL_SOURCE;
        disable.sink.entity   = bridge_entity_id;
        disable.sink.index    = 0;
        disable.sink.flags    = MEDIA_PAD_FL_SINK;
        disable.flags         = 0;  // disabled
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

    // Set subdev format on selected sensor
    if (subdev_fd >= 0) {
        struct v4l2_subdev_format fmt = {0};
        fmt.which         = V4L2_SUBDEV_FORMAT_ACTIVE;
        fmt.pad           = 0;
        fmt.format.width  = width;
        fmt.format.height = height;
        fmt.format.code   = 0x3001;  // MEDIA_BUS_FMT_SBGGR8_1X8
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

        // Check if there are more frames queued — if so, skip this one
        struct v4l2_buffer peek = {0};
        peek.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        peek.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cam->fd, VIDIOC_DQBUF, &peek) == 0) {
            // There's a newer frame, discard current and use newer
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
  mrb_v4l2_wrapper* wrapper = (mrb_v4l2_wrapper*) payload;
  cleanup(wrapper);
  free(payload);
}

static struct mrb_data_type mrb_v4l2_type = { "Camera", mrb_v4l2_type_free };

mrb_v4l2_wrapper* mrb_v4l2_get(mrb_state* mrb, mrb_value self)
{
  mrb_v4l2_wrapper* wrapper = (mrb_v4l2_wrapper*)DATA_PTR(self);
  if (!wrapper) {
    mrb_raise(mrb, E_ARGUMENT_ERROR , "uninitialized camera data") ;
  }
  
  return wrapper;
}

mrb_value mrb_v4l2_device_init(mrb_state* mrb, mrb_value self)
{
  char* device;
  mrb_value rwidth;
  mrb_value rheight;
  mrb_value options = mrb_nil_value();
  mrb_get_args(mrb, "zoo|H", &device, &rwidth, &rheight, &options);

  // Parse facing option
  camera_facing facing = CAMERA_REAR;
  if (!mrb_nil_p(options)) {
      mrb_value rfacing = mrb_hash_get(mrb, options, mrb_symbol_value(mrb_intern_lit(mrb, "facing")));
      if (!mrb_nil_p(rfacing)) {
          const char* fs = mrb_sym2name(mrb, mrb_symbol(rfacing));
          if (strcmp(fs, "front") == 0) facing = CAMERA_FRONT;
      }
  }

  int width = mrb_int(mrb, rwidth);
  int height = mrb_int(mrb, rheight);
  printf("%s - %d %d\n", device, width, height);

  int fd = open(device, O_RDWR | O_NONBLOCK, 0);
  
  if (fd < 0) {
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "Can't open %s: %s (errno: %d)", 
             device, strerror(errno), errno); 
    mrb_raise(mrb, E_STANDARD_ERROR, error_msg);
  }

  int media_fd = open("/dev/media1", O_RDWR);
  if (media_fd >= 0) {
      int subdev_fd = open("/dev/v4l-subdev1", O_RDWR);
      setup_media_pipeline(media_fd, subdev_fd, width, height, facing);
      if (subdev_fd >= 0) close(subdev_fd);
      close(media_fd);
  }

  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // Common webcam format
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) mrb_raise(mrb, E_STANDARD_ERROR, "can't apply ioctl settings to device");
  mrb_value obj = mrb_funcall(mrb, self, "new", 0, NULL);

  mrb_v4l2_wrapper* wrapper = malloc(sizeof(mrb_v4l2_wrapper));
  wrapper->fd = fd;
  wrapper->facing = facing;
  wrapper->width = fmt.fmt.pix.width;
  wrapper->height = fmt.fmt.pix.height;
  wrapper->pixelformat = fmt.fmt.pix.pixelformat;
  wrapper->rgba_size = wrapper->width * wrapper->height * 4;
  
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

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) mrb_raise(mrb, E_STANDARD_ERROR, "can't request buffer");
    cam->buffer_count = req.count;

    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) mrb_raise(mrb, E_STANDARD_ERROR, "can't query video buffer");

        cam->buffers[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, buf.m.offset);
        cam->buffers[i].length = buf.length;

        if (cam->buffers[i].start == MAP_FAILED) mrb_raise(mrb, E_STANDARD_ERROR, "can't map buffer");

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) mrb_raise(mrb, E_STANDARD_ERROR, "failed to queue buffer");
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) mrb_raise(mrb, E_STANDARD_ERROR, "failed to start stream");

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
  if (cam->fd == -1) mrb_raise(mrb, E_STANDARD_ERROR, "Camera already closed!");
  cleanup(cam);  // cleanup handles everything now
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

    // Copy front buffer into mruby string
    mrb_value rstr = mrb_str_new(mrb, (const char*)cam->front, cam->rgba_size);
    cam->ready = false;
    pthread_mutex_unlock(&cam->mutex);

    return rstr;
}

// TODO: this is totally for pinephone..
mrb_value mrb_v4l2_device_switch_camera(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);
    
    // No-op on desktop — no media controller
    int media_fd = open("/dev/media1", O_RDWR);
    if (media_fd < 0) {
        fprintf(stderr, "no media controller, switch_camera is a no-op\n");
        return mrb_false_value();
    }
    // Stop current stream
    cam->running = false;
    pthread_join(cam->thread, NULL);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);

    cam->facing = (cam->facing == CAMERA_REAR) ? CAMERA_FRONT : CAMERA_REAR;

    const char* subdev_path = (cam->facing == CAMERA_FRONT) ?
                               "/dev/v4l-subdev2" : "/dev/v4l-subdev1";
    int subdev_fd = open(subdev_path, O_RDWR);
    setup_media_pipeline(media_fd, subdev_fd, cam->width, cam->height, cam->facing);
    if (subdev_fd >= 0) close(subdev_fd);
    close(media_fd);

    ioctl(cam->fd, VIDIOC_STREAMON, &type);
    cam->running = true;
    cam->ready   = false;
    pthread_create(&cam->thread, NULL, camera_capture_thread, cam);

    return mrb_true_value();
}


mrb_value mrb_v4l2_device_width(mrb_state* mrb, mrb_value self)
{
  mrb_v4l2_wrapper* wrapper = mrb_v4l2_get(mrb, self);\
  return mrb_int_value(mrb, wrapper->width);
}
mrb_value mrb_v4l2_device_height(mrb_state* mrb, mrb_value self)
{
  mrb_v4l2_wrapper* wrapper = mrb_v4l2_get(mrb, self);\
  return mrb_int_value(mrb, wrapper->height);
}
void mrb_mruby_v4l2_gem_init(mrb_state* mrb)
{
  struct RClass* mod = mrb_define_module(mrb, "V4L2");
  struct RClass* klass = mrb_define_class_under(mrb, mod, "Camera", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_class_method(mrb, klass, "init", mrb_v4l2_device_init, MRB_ARGS_REQ(3) | MRB_ARGS_OPT(1));
  mrb_define_method(mrb, klass, "open", mrb_v4l2_device_open, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "close", mrb_v4l2_device_close, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "frame", mrb_v4l2_device_req_frame, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "width", mrb_v4l2_device_width, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "height", mrb_v4l2_device_height, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "switch", mrb_v4l2_device_switch_camera, MRB_ARGS_NONE());
}
void mrb_mruby_v4l2_gem_final(mrb_state* mrb)
{

}
#endif
