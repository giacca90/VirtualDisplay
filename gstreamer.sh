gst-launch-1.0 -v \
  ximagesrc use-damage=0 startx=0 starty=0 endx=1920 endy=1080 ! \
  video/x-raw,framerate=30/1 ! \
  videoconvert ! queue ! \
  vp8enc deadline=1 ! rtpvp8pay ! \
  application/x-rtp,media=video,encoding-name=VP8,payload=96 ! \
  webrtcbin name=sendrecv stun-server=stun://stun.l.google.com:19302