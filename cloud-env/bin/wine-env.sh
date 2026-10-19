# Common environment for running MSYS2 UCRT64 Windows binaries under Wine.
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEPREFIX=/opt/wineprefix
export WINEARCH=win64
export MSYS2_UCRT64=/opt/msys2/ucrt64
export DISPLAY="${DISPLAY:-:99}"
if ! [ -e /tmp/.X11-unix/X99 ]; then
  (Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 &) ; sleep 1
fi
