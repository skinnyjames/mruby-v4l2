#ifndef MRUBY_V4L2_H
#define MRUBY_V4L2_H
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

typedef enum {
    CAMERA_REAR  = 0,
    CAMERA_FRONT = 1
} camera_facing;

typedef struct {
    void* start;
    size_t length;
} v4l2_buf_map;

typedef struct mrb_v4l2 {
  int fd;
  unsigned char* front;
  unsigned char* back;
  camera_facing facing;
  uint32_t pixelformat;
  size_t rgba_size;
  pthread_t thread;
  pthread_mutex_t mutex;
  bool ready;
  bool running;
  v4l2_buf_map buffers[4];
  int buffer_count;
  int width;
  int height;
} mrb_v4l2_wrapper;

void mrb_mruby_v4l2_gem_init(mrb_state* mrb);
void mrb_mruby_v4l2_gem_final(mrb_state* mrb);
#endif
