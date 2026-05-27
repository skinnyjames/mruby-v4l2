module V4L2
  DESKTOP = {
    pixel_format: :yuyv
  }.freeze

  PINEPHONE_REAR = {
    media_dev:         "/dev/media1",
    bridge_name:       "sun6i-csi-bridge",
    subdev_dev:        "/dev/v4l-subdev1",
    other_subdev_dev:  "/dev/v4l-subdev2",
    sensor_name:       "ov5640",
    other_sensor_name: "gc2145",
    pixel_format:      :sbggr8,
    bus_fmt_code:      0x3001,
  }.freeze

  PINEPHONE_FRONT = {
    media_dev:         "/dev/media1",
    bridge_name:       "sun6i-csi-bridge",
    subdev_dev:        "/dev/v4l-subdev2",
    other_subdev_dev:  "/dev/v4l-subdev1",
    sensor_name:       "gc2145",
    other_sensor_name: "ov5640",
    pixel_format:      :yuyv,
    bus_fmt_code:      0x2006,
  }.freeze

  PINEPHONE_PRO_REAR = {
    media_dev:         "/dev/media1",
    bridge_name:       "sun6i-csi-bridge",
    subdev_dev:        "/dev/v4l-subdev1",
    other_subdev_dev:  "/dev/v4l-subdev2",
    sensor_name:       "ov64a40",
    other_sensor_name: "ov8858",
    pixel_format:      :sbggr8,
    bus_fmt_code:      0x3001,
  }.freeze

  PINEPHONE_PRO_FRONT = {
    media_dev:         "/dev/media1",
    bridge_name:       "sun6i-csi-bridge",
    subdev_dev:        "/dev/v4l-subdev2",
    other_subdev_dev:  "/dev/v4l-subdev1",
    sensor_name:       "ov8858",
    other_sensor_name: "ov64a40",
    pixel_format:      :yuyv,
    bus_fmt_code:      0x2006,
  }.freeze
end