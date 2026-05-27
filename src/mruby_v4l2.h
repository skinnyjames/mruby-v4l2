#ifndef MRUBY_MRB_V4L2_H
#define MRUBY_MRB_V4L2_H
#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/data.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <pthread.h>

#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <fcntl.h>      // For open() and O_RDWR
#include <unistd.h>     // For close()
#include <sys/ioctl.h>  // For ioctl()
#include <sys/mman.h>   // For mmap() and munmap()
#include <malloc.h>
#include <stdbool.h>

typedef struct {
    void* start;
    size_t length;
} mrb_v4l2_buf_map;

enum mrb_v4l2_pix_format {
    /* RGB and Greyscale Formats */
  MRB_V4L2_UINT_GREY    = 0x59455247,
  MRB_V4L2_UINT_Y10     = 0x20303159,
  MRB_V4L2_UINT_Y12     = 0x20323159,
  MRB_V4L2_UINT_Y16     = 0x20363159,
  MRB_V4L2_UINT_RGB565  = 0x50424752,
  MRB_V4L2_UINT_RGB24   = 0x33424752,
  MRB_V4L2_UINT_BGR24   = 0x33524742,
  MRB_V4L2_UINT_RGB32   = 0x34424752,
  MRB_V4L2_UINT_BGR32   = 0x34524742,
  MRB_V4L2_UINT_ARGB32  = 0x34324142,
  MRB_V4L2_UINT_XRGB32  = 0x34325842,

  /* YUV Formats (Packed & Planar) */
  MRB_V4L2_UINT_YUYV    = 0x56595559,
  MRB_V4L2_UINT_UYVY    = 0x59565955,
  MRB_V4L2_UINT_YVYU    = 0x55595659,
  MRB_V4L2_UINT_VYUY    = 0x59555956,
  MRB_V4L2_UINT_NV12    = 0x3231564E,
  MRB_V4L2_UINT_NV21    = 0x3132564E,
  MRB_V4L2_UINT_NV16    = 0x3631564E,
  MRB_V4L2_UINT_NV61    = 0x3136564E,
  MRB_V4L2_UINT_YUV420  = 0x30323449,
  MRB_V4L2_UINT_YVU420  = 0x32315659,
  MRB_V4L2_UINT_YUV422P = 0x50323234,

  /* Compressed Streams */
  MRB_V4L2_UINT_MJPEG   = 0x47504A4D,
  MRB_V4L2_UINT_JPEG    = 0x4745504A,
  MRB_V4L2_UINT_H264    = 0x34363248,
  MRB_V4L2_UINT_HEVC    = 0x43564548,
  MRB_V4L2_UINT_MPEG    = 0x4745504D,
  MRB_V4L2_UINT_VP8     = 0x30385056,
  MRB_V4L2_UINT_VP9     = 0x30395056,

  /* Raw Bayer Formats */
  MRB_V4L2_UINT_SBGGR8  = 0x31384142,
  MRB_V4L2_UINT_SGBRG8  = 0x47524247,
  MRB_V4L2_UINT_SGRBG8  = 0x47425247,
  MRB_V4L2_UINT_SRGGB8  = 0x42474752,
  MRB_V4L2_UINT_SBGGR10 = 0x30314742,
  MRB_V4L2_UINT_SGBRG10 = 0x30314247,
  MRB_V4L2_UINT_SGRBG10 = 0x30314142,
  MRB_V4L2_UINT_SRGGB10 = 0x30314752,
  MRB_V4L2_UINT_SBGGR12 = 0x32314742,
  MRB_V4L2_UINT_SRGGB12 = 0x32314752
};


typedef struct mrb_v4l2 {
  int fd;

  const char* media_dev;
  const char* sensor_name;
  const char* bridge_name;
  const char* active;

  uint32_t    busformat;
  uint32_t    pixelformat;

  size_t rgba_size;
  pthread_t thread;
  pthread_mutex_t mutex;

  bool ready;
  bool running;
  unsigned char* front;
  unsigned char* back;

  mrb_v4l2_buf_map buffers[4];
  int buffer_count;

  int width;
  int height;
} mrb_v4l2_wrapper;

void mrb_mruby_v4l2_gem_init(mrb_state* mrb);
void mrb_mruby_v4l2_gem_final(mrb_state* mrb);
#endif
