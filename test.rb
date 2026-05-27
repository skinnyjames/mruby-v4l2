camera = V4L2::Camera.init("/dev/video0", 200, 200, V4L2::DESKTOP)
camera.open

while true
  usleep 5000
  p camera.frame
end