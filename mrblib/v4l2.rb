module V4L2
  # BUS FORMATS
  BUS_SBGGR8_1X8 = 0x3001
  BUS_UYVY8_2X8 = 0x2006
  
  PIX_GREY    = 0x59455247
  PIX_Y10     = 0x20303159
  PIX_Y12     = 0x20323159
  PIX_Y16     = 0x20363159
  PIX_RGB565  = 0x50424752
  PIX_RGB24   = 0x33424752
  PIX_BGR24   = 0x33524742
  PIX_RGB32   = 0x34424752
  PIX_BGR32   = 0x34524742
  PIX_ARGB32  = 0x34324142
  PIX_XRGB32  = 0x34325842

  # YUV
  PIX_YUYV    = 0x56595559
  PIX_UYVY    = 0x59565955
  PIX_YVYU    = 0x55595659
  PIX_VYUY    = 0x59555956
  PIX_NV12    = 0x3231564E
  PIX_NV21    = 0x3132564E
  PIX_NV16    = 0x3631564E
  PIX_NV61    = 0x3136564E
  PIX_YUV420  = 0x30323449
  PIX_YVU420  = 0x32315659
  PIX_YUV422P = 0x50323234

  # Stream
  PIX_MJPEG   = 0x47504A4D
  PIX_JPEG    = 0x4745504A
  PIX_H264    = 0x34363248
  PIX_HEVC    = 0x43564548
  PIX_MPEG    = 0x4745504D
  PIX_VP8     = 0x30385056
  PIX_VP9     = 0x30395056

  # Bayer
  PIX_SBGGR8  = 0x31384142
  PIX_SGBRG8  = 0x47524247
  PIX_SGRBG8  = 0x47425247
  PIX_SRGGB8  = 0x42474752
  PIX_SBGGR10 = 0x30314742
  PIX_SGBRG10 = 0x30314247
  PIX_SGRBG10 = 0x30314142
  PIX_SRGGB10 = 0x30314752
  PIX_SBGGR12 = 0x32314742
  PIX_SRGGB12 = 0x32314752

  DESKTOP = {
    pixel_format: PIX_YUYV
  }.freeze

  PINEPHONE_FRONT = {
    media:        "/dev/media1",
    bridge:       "sun6i-csi-bridge",
    sensor:       "gc2145",
    active:       "ov5640",      # disable this on init
    pixel_format: PIX_UYVY,
    bus_format:   BUS_UYVY8_2X8,
  }.freeze

  PINEPHONE_REAR = {
    media:        "/dev/media1",
    bridge:       "sun6i-csi-bridge",
    sensor:       "ov5640",
    active:       "gc2145",      # disable this on init
    pixel_format: PIX_SBGGR8,
    bus_format:   BUS_SBGGR8_1X8,
  }.freeze

  PINEPHONE_PRO_REAR = {
    media:         "/dev/media1",
    bridge:       "sun6i-csi-bridge",
    sensor:       "ov64a40",
    pixel_format:      PIX_SBGGR8,
    bus_format:      BUS_SBGGR8_1X8,
  }.freeze

  PINEPHONE_PRO_FRONT = {
    media:         "/dev/media1",
    bridge:       "sun6i-csi-bridge",
    sensor:       "ov8858",
    pixel_format:      PIX_UYVY,
    bus_format:      BUS_UYVY8_2X8,
  }.freeze
end