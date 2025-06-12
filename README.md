WebRTCPlayer README
====================

WebRTCPlayer is a simple application for playing video files via WebRTC to a browser.

## Build

```
git clone git@github.com:MonocleSecurity/WebRTCPlayer.git --recursive
cd WebRTCPlayer
VCPKG_FORCE_SYSTEM_BINARIES=1 cmake -G"Unix Makefiles" .
make
```

## Run

./WebRTCPlayer video.mp4
