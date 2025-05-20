gst-launch-1.0 -v \
  ximagesrc use-damage=0 ! video/x-raw,framerate=30/1 ! \
  videoconvert ! \
  avenc_mpeg1video bitrate=2000 ! \
  mpegtsmux ! \
  tcpclientsink host=127.0.0.1 port=9999