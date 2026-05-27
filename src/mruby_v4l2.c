#ifndef MRUBY_V4L2
#define MRUBY_V4L2
#include "mruby_v4l2.h"
#include <string.h>
#include <errno.h>

/**
  WRAPPER FUNCTIONS
*/
#include <sys/sysmacros.h>  // makedev, major, minor

char* find_entity_devnode(int media_fd, const char* entity_name)
{
    struct media_entity_desc entity = {0};
    
    for (int id = 0; ; id++)
    {
        entity.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) < 0) break;
        
        if (!strstr(entity.name, entity_name)) continue;

        // got a match - resolve devnode from major/minor
        char syspath[256];
        snprintf(syspath, sizeof(syspath), "/sys/dev/char/%u:%u/uevent",
            entity.dev.major, entity.dev.minor);

        FILE* f = fopen(syspath, "r");
        if (!f) return NULL;

        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            if (strncmp(line, "DEVNAME=", 8) == 0)
            {
                fclose(f);
                char* devname = line + 8;
                devname[strcspn(devname, "\n")] = 0;  // strip newline
                
                char devpath[256];
                snprintf(devpath, sizeof(devpath), "/dev/%s", devname);
                return strdup(devpath);  // caller must free
            }
        }
        fclose(f);
    }
    return NULL;
}

void cleanup(mrb_v4l2_wrapper* camera)
{
  if (camera->fd == -1) return;

  if (camera->running)
  {
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

  for (int i = 0; i < camera->buffer_count; i++) 
  {
    if (camera->buffers[i].start && camera->buffers[i].start != MAP_FAILED)
    {
      munmap(camera->buffers[i].start, camera->buffers[i].length);
    }
  }

  close(camera->fd);
  camera->fd = -1;
}

static void mrb_v4l2_type_free(mrb_state* mrb, void* payload)
{
    mrb_v4l2_wrapper* wrapper = (mrb_v4l2_wrapper*)payload;
    cleanup(wrapper);

    free(wrapper);
}

static struct mrb_data_type mrb_v4l2_type = { "Camera", mrb_v4l2_type_free };

mrb_v4l2_wrapper* mrb_v4l2_get(mrb_state* mrb, mrb_value self)
{
    mrb_v4l2_wrapper* wrapper = (mrb_v4l2_wrapper*)DATA_PTR(self);
    if (!wrapper)
        mrb_raise(mrb, E_ARGUMENT_ERROR, "uninitialized camera data");
    return wrapper;
}

/**
  COLOR CONVERSIONS
  We are converting a given pixel format to an RGBA array
  This is more portable and can be consumed correctly by hokusai/raylib.
**/
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


void* mrb_v4l2_camera_capture_thread(void* arg)
{
  mrb_v4l2_wrapper* cam = (mrb_v4l2_wrapper*)arg;

  while (cam->running) 
  {
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) 
    {
      if (errno == EAGAIN) continue;
      break;
    }

    // If there's a newer frame queued, skip this one
    struct v4l2_buffer peek = {0};
    peek.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    peek.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam->fd, VIDIOC_DQBUF, &peek) == 0) 
    {
      ioctl(cam->fd, VIDIOC_QBUF, &buf);
      buf = peek;
    }

    unsigned char* src = (unsigned char*)cam->buffers[buf.index].start;

    switch (cam->pixelformat) 
    {
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

/**
  INIT
*/
// Parse a string option from a Ruby hash by key name. Returns NULL if absent.
static const char* v4l2_parse_str_opt(mrb_state* mrb, mrb_value options, const char* key)
{
    mrb_value v = mrb_hash_get(mrb, options, mrb_symbol_value(mrb_intern_cstr(mrb, key)));
    return mrb_nil_p(v) ? NULL : mrb_str_to_cstr(mrb, v);
}

// Parse a uint32 option from a Ruby hash by key name. Returns def if absent.
static uint32_t v4l2_parse_uint_opt(mrb_state* mrb, mrb_value options, const char* key, uint32_t def)
{
    mrb_value v = mrb_hash_get(mrb, options, mrb_symbol_value(mrb_intern_cstr(mrb, key)));
    return mrb_nil_p(v) ? def : (uint32_t)mrb_int(mrb, v);
}


/**
  MEDIA PIPELINE
  For embedded devices / mobile
*/
int setup_media_pipeline(mrb_v4l2_wrapper* cam, int media_fd)
{
  int sensor_id = -1;
  int active_id = -1;
  int bridge_id = -1;

  struct media_entity_desc entity = {0};
  for (int id = 0; ; id++) 
  {
    entity.id = id | MEDIA_ENT_ID_FLAG_NEXT;
    if (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) < 0) break;

    if (cam->sensor_name && strstr(entity.name, cam->sensor_name)) sensor_id = entity.id;
    if (cam->active      && strstr(entity.name, cam->active))      active_id = entity.id;
    if (cam->bridge_name && strstr(entity.name, cam->bridge_name)) bridge_id = entity.id;
  }

  if (sensor_id < 0 || bridge_id < 0) 
  {
    fprintf(stderr, "couldn't find entities\n");
    return -1;
  }

  // Disable competing sensor
  if (active_id >= 0) 
  {
    struct media_link_desc disable = {0};
    disable.source.entity = active_id;
    disable.source.index  = 0;
    disable.source.flags  = MEDIA_PAD_FL_SOURCE;
    disable.sink.entity   = bridge_id;
    disable.sink.index    = 0;
    disable.sink.flags    = MEDIA_PAD_FL_SINK;
    disable.flags         = 0;
    ioctl(media_fd, MEDIA_IOC_SETUP_LINK, &disable);
  }

  // Enable selected sensor
  struct media_link_desc link = {0};
  link.source.entity = sensor_id;
  link.source.index  = 0;
  link.source.flags  = MEDIA_PAD_FL_SOURCE;
  link.sink.entity   = bridge_id;
  link.sink.index    = 0;
  link.sink.flags    = MEDIA_PAD_FL_SINK;
  link.flags         = MEDIA_LNK_FL_ENABLED;

  if (ioctl(media_fd, MEDIA_IOC_SETUP_LINK, &link) < 0) 
  {
    fprintf(stderr, "failed to enable link: %s\n", strerror(errno));
    return -1;
  }

  cam->active = cam->sensor_name;

  // Set sensor subdev format
  char* sensor_devnode = find_entity_devnode(media_fd, cam->sensor_name);
  if (sensor_devnode)
  {
    int subdev_fd = open(sensor_devnode, O_RDWR);
    free(sensor_devnode);

    if (subdev_fd >= 0)
    {
      struct v4l2_subdev_format fmt = {0};
      fmt.which         = V4L2_SUBDEV_FORMAT_ACTIVE;
      fmt.pad           = 0;
      fmt.format.width  = cam->width;
      fmt.format.height = cam->height;
      fmt.format.code   = cam->busformat;
      fmt.format.field  = V4L2_FIELD_NONE;

      if (ioctl(subdev_fd, VIDIOC_SUBDEV_S_FMT, &fmt) < 0)
      {
        fprintf(stderr, "failed to set sensor fmt: %s\n", strerror(errno));
        close(subdev_fd);
        return -1;
      }

      cam->width  = fmt.format.width;
      cam->height = fmt.format.height;
      close(subdev_fd);
    }
  }

  // Set bridge subdev pads
  char* bridge_devnode = find_entity_devnode(media_fd, cam->bridge_name);
  if (bridge_devnode)
  {
    int bridge_fd = open(bridge_devnode, O_RDWR);
    free(bridge_devnode);

    if (bridge_fd >= 0)
    {
      struct v4l2_subdev_format bfmt = {0};
      bfmt.which         = V4L2_SUBDEV_FORMAT_ACTIVE;
      bfmt.format.width  = cam->width;
      bfmt.format.height = cam->height;
      bfmt.format.code   = cam->busformat;
      bfmt.format.field  = V4L2_FIELD_NONE;

      bfmt.pad = 0;
      if (ioctl(bridge_fd, VIDIOC_SUBDEV_S_FMT, &bfmt) < 0)
      {
        fprintf(stderr, "failed to set bridge sink pad: %s\n", strerror(errno));
        close(bridge_fd);
        return -1;
      }
      bfmt.pad = 1;
      ioctl(bridge_fd, VIDIOC_SUBDEV_S_FMT, &bfmt);

      close(bridge_fd);
    }
  }

  return 0;
}

mrb_value mrb_v4l2_device_init(mrb_state* mrb, mrb_value self)
{
  char*    device;
  mrb_value rwidth, rheight;
  mrb_value options = mrb_nil_value();
  mrb_get_args(mrb, "zoo|H", &device, &rwidth, &rheight, &options);

  int width  = mrb_int(mrb, rwidth);
  int height = mrb_int(mrb, rheight);

  int fd = open(device, O_RDWR | O_NONBLOCK, 0);
  if (fd < 0) 
  {
      char error_msg[256];
      snprintf(error_msg, sizeof(error_msg), "Can't open %s: %s (errno: %d)",
                device, strerror(errno), errno);
      mrb_raise(mrb, E_STANDARD_ERROR, error_msg);
  }

  const char* media_dev    = NULL;
  const char* subdev_dev   = NULL;
  const char* sensor_name  = NULL;
  const char* bridge_name  = NULL;
  const char* bridge_subdev = NULL;
  uint32_t    bus_fmt_code = MEDIA_BUS_FMT_SBGGR8_1X8;
  uint32_t    pixelformat  = V4L2_PIX_FMT_YUYV;

  // option parsing.
  if (!mrb_nil_p(options)) 
  {
    media_dev    = v4l2_parse_str_opt(mrb, options, "media");
    sensor_name  = v4l2_parse_str_opt(mrb, options, "sensor");
    bridge_name  = v4l2_parse_str_opt(mrb, options, "bridge");
    bus_fmt_code = v4l2_parse_uint_opt(mrb, options, "bus_format", MEDIA_BUS_FMT_SBGGR8_1X8);
    pixelformat  = v4l2_parse_uint_opt(mrb, options, "pixel_format", V4L2_PIX_FMT_YUYV);
  }

  // setup wrapper with requested dimensions and formats
  mrb_v4l2_wrapper* wrapper = malloc(sizeof(mrb_v4l2_wrapper));
  wrapper->fd          = fd;
  wrapper->width       = width;
  wrapper->height      = height;
  wrapper->pixelformat = pixelformat;
  wrapper->busformat   = bus_fmt_code;
  wrapper->rgba_size   = wrapper->width * wrapper->height * 4;
  wrapper->media_dev   = media_dev;
  wrapper->sensor_name = sensor_name;
  wrapper->bridge_name = bridge_name;
  wrapper->active = NULL;

  // handle media pipeline for embedded devices
  // we want to fail if we can't open the descriptors.
  if (wrapper->media_dev)
  {
    int mediafd = open(media_dev, O_RDWR);
    if (mediafd < 0)
    {
      perror("Can't open media device");
      mrb_raise(mrb, E_ARGUMENT_ERROR, "Media device can't be opened");
    }

    if (setup_media_pipeline(wrapper, mediafd) < 0) 
    {
      mrb_raise(mrb, E_STANDARD_ERROR, "Can't setup media pipeline");
    }
  }

  // Configure device with requested attributes
  struct v4l2_format fmt = {0};
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = wrapper->width;
  fmt.fmt.pix.height      = wrapper->height;
  fmt.fmt.pix.pixelformat = wrapper->pixelformat;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;

  if (ioctl(wrapper->fd, VIDIOC_S_FMT, &fmt) < 0)
      mrb_raise(mrb, E_STANDARD_ERROR, "can't apply ioctl settings to device");

  // Finally, update wrapper with negotiated stuff.
  wrapper->width        = fmt.fmt.pix.width;
  wrapper->height       = fmt.fmt.pix.height;
  wrapper->pixelformat  = fmt.fmt.pix.pixelformat;
  wrapper->rgba_size    = wrapper->width * wrapper->height * 4;

  mrb_value obj = mrb_funcall(mrb, self, "new", 0, NULL);
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

  for (int i = 0; i < req.count; i++)
  {
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;

    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) mrb_raise(mrb, E_STANDARD_ERROR, "can't query video buffer");

    cam->buffers[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    cam->buffers[i].length = buf.length;

    if (cam->buffers[i].start == MAP_FAILED) mrb_raise(mrb, E_STANDARD_ERROR, "can't map buffer");

    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) mrb_raise(mrb, E_STANDARD_ERROR, "failed to queue buffer");
  }

  // all buffers queued, now we want to do a threaded loop for capturing the camera image
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0)
  {
    perror("Cannot start stream\n");
    mrb_raise(mrb, E_STANDARD_ERROR, "failed to start stream");
  }

  // setup our double buffer here
  // we can populate one while one is being read, or something
  cam->front = malloc(cam->rgba_size);
  if (!cam->front) mrb_raise(mrb, E_STANDARD_ERROR, "out of memory");
  cam->back = malloc(cam->rgba_size);
  if (!cam->back) mrb_raise(mrb, E_STANDARD_ERROR, "out of memory");

  cam->running = true;
  cam->ready   = false;
  pthread_mutex_init(&cam->mutex, NULL);
  pthread_create(&cam->thread, NULL, mrb_v4l2_camera_capture_thread, cam);

  return mrb_true_value();
}

mrb_value mrb_v4l2_device_req_frame(mrb_state* mrb, mrb_value self)
{
  mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);

  pthread_mutex_lock(&cam->mutex);
  if (!cam->ready) 
  {
    pthread_mutex_unlock(&cam->mutex);
    return mrb_nil_value();
  }

  mrb_value rstr = mrb_str_new(mrb, (const char*)cam->front, cam->rgba_size);
  cam->ready = false;
  pthread_mutex_unlock(&cam->mutex);

  return rstr;
}

mrb_value mrb_v4l2_device_close(mrb_state* mrb, mrb_value self)
{
  mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);

  cleanup(cam);
  return mrb_nil_value();
}

mrb_value mrb_v4l2_device_width(mrb_state* mrb, mrb_value self)
{
  mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);
  return mrb_int_value(mrb, cam->width);
}

mrb_value mrb_v4l2_device_height(mrb_state* mrb, mrb_value self)
{  
  mrb_v4l2_wrapper* cam = mrb_v4l2_get(mrb, self);
  return mrb_int_value(mrb, cam->height);
}

void mrb_mruby_v4l2_gem_init(mrb_state* mrb)
{
  struct RClass* mod   = mrb_define_module(mrb, "V4L2");
  struct RClass* klass = mrb_define_class_under(mrb, mod, "Camera", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_class_method(mrb, klass, "init",   mrb_v4l2_device_init,          MRB_ARGS_REQ(3) | MRB_ARGS_OPT(1));

  mrb_define_method(mrb, klass, "width", mrb_v4l2_device_width, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "height", mrb_v4l2_device_height, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "open",         mrb_v4l2_device_open,          MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "close",        mrb_v4l2_device_close,         MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "frame",        mrb_v4l2_device_req_frame,     MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "width",        mrb_v4l2_device_width,         MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "height",       mrb_v4l2_device_height,        MRB_ARGS_NONE());
}

void mrb_mruby_v4l2_gem_final(mrb_state* mrb)
{
}
#endif