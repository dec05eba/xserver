Divergence from upstream
========================

This fork of the X.Org X server adds server side compositing, per-monitor
framebuffers, HDR, NVIDIA support in the modesetting driver, per-monitor DPI and synchronized window resizing. This file
describes what is different from upstream.

Note that when installing this Xorg server version you also need to rebuild
and install the latest versions of Xorg drivers from source, such as
[xf86-input-libinput](https://github.com/dec05eba/xf86-input-libinput).

Compositing
-----------

With the modesetting driver every monitor has its own framebuffer and the X
server composites the windows itself, without tearing. Other compositors
aren't needed, which also allows for lower latency while compositing.
Fullscreen windows bypass compositing for maximum performance, and variable
refresh rate works with them.

- Windows with an alpha channel are transparent and `_NET_WM_WINDOW_OPACITY`
  is supported, so you don't need a compositor such as picom.
- The X server owns the `_NET_WM_CM_S<screen>` selection, so picom and similar
  compositors refuse to start. This is needed for proper multimonitor support and HDR.
  It's technically possible to add proper multimonitor support and HDR to existing compositors
  (such as picom) but they would need to be modified to handle these cases and wouldn't be
  able to utilize certain optimizations that only the display server (the Xorg server) can,
  such as doing tonemapping with the display processing unit.

  Window managers with a built-in compositor, such as KWin and xfwm4,
  run with their compositor disabled. Mutter based window managers always composite
  and may refuse to start unless they are started with `--replace`.

Compositing is on by default when the modesetting driver uses glamor. To turn
it off, set the `Compositing` option in the Device section of xorg.conf:

    Section "Device"
        Identifier "GPU"
        Driver     "modesetting"
        Option     "Compositing" "false"
    EndSection

All monitors then share one framebuffer like in upstream, and a separate
compositor such as picom can be used. Without compositing you lose tear-free
output, transparency without a separate compositor, HDR windows that aren't
fullscreen, synchronized window resizing.

`TearFree` is the old name of this option. It still works but is deprecated,
and if both are set `Compositing` is used.

Limitations:

- Drawing directly on the root window, other than its background, isn't
  shown. Desktop wallpapers set with tools like `xsetroot` and `feh` work.
- Reading the root window works with GetImage (screenshots, `xwd`, ffmpeg
  x11grab), but not with CopyArea or Render.
- Fullscreen windows on monitors connected to a secondary GPU (reverse PRIME)
  are copied by that GPU instead of scanned out directly. The exception is
  applications started with NVIDIA's `prime-run` on a monitor connected to
  the NVIDIA GPU, when that GPU has an overlay plane for the monitor.
- With a software cursor, fullscreen windows on the monitor with the cursor
  don't bypass compositing.

Window resizing
---------------

Windows are resized without their content jumping, stretching or showing
garbage, with any window manager. While a window is being resized the X server
keeps showing its previous frame until the application has drawn one at the
new size.

- This works best with applications that support `_NET_WM_SYNC_REQUEST`
  (GTK, Qt, Chromium/Electron and others).
- Applications without `_NET_WM_SYNC_REQUEST` (for example Alacritty, mpv and
  most games) can sometimes still show a frame that was drawn for the previous
  size.
- A window can only be resized as fast as its application draws. If an
  application doesn't draw the new size within 200 milliseconds the window is
  shown anyway.

The X server also sends `_NET_WM_FRAME_DRAWN` and `_NET_WM_FRAME_TIMINGS` to
applications that use them, such as GTK, so they draw at the refresh rate of
the monitor instead of 60 frames per second.

Performance
-----------

This X server has been compared with the upstream X server and with KWin and
Mutter on Wayland. It's smoother than all of them in most cases and as smooth
in the rest, never less smooth. Games framerate are the same in each, but the
framerate at which they are presented on the screen differs.

Fullscreen windows are scanned out directly: the monitor shows the
application's own buffer, without compositing or copying it. This works when:

- the window covers a whole monitor exactly and isn't transparent. Windows on
  top of it, such as notifications and menus, are shown on an overlay plane
  when the GPU has one for that monitor. Without one the monitor is composited
  while they are visible,
- the window isn't being resized,
- the monitor isn't rotated or scaled and the cursor isn't a software cursor,
- the application shows its frames through DRI3 and Present, which Vulkan and
  OpenGL applications do,
- the GPU can scan out the buffer format the application renders into.

Fullscreen HDR windows (or SDR windows when HDR is enabled) are scanned out directly too.

Games that aren't fullscreen are scanned out directly as well, on an overlay
plane, when the GPU has one for that monitor and no other window covers the
game. Windows that are being moved are shown on the overlay plane too, so they
follow the cursor smoothly even while a game keeps the GPU fully busy.

X11 rendering has also been made faster, so the desktop stays smooth under heavy
GPU load.

Monitors that are composited are composed just before each vblank, so a new
frame reaches the screen at the next vblank and frame pacing stays even.

The X server runs its threads and its GPU work at high priority, so busy
applications are less likely to make the desktop and games stutter. That needs
the `CAP_SYS_NICE` capability, which installing Xorg gives it. The
`cap_sys_nice` meson option turns this off.

Applications with vsync turned off never tear. Their newest frame is shown at
each vblank and they still run uncapped. To show their frames immediately with
async flips, which can tear, set the `AllowTearing` option to true in the
Device section of xorg.conf.

Applications with vsync turned on whose windows are minimized, completely
covered or off screen, for example on another workspace, are slowed down to 1
frame per second, so they don't take GPU time from what you're looking at.
Like on Wayland, applications with vsync turned off keep running at full speed,
so applications like OBS keep recording while their window is hidden. The
`HiddenWindowFPS` option in the Device section of xorg.conf sets that rate, and
0 lets them run as if they were visible.

On laptops with two GPUs, fullscreen applications started with NVIDIA's
`prime-run` on a monitor connected to the NVIDIA GPU are scanned out straight
from the NVIDIA GPU's memory. They avoid all copies, so they perform the same
as on a computer with only that GPU. See [Multiple GPUs](#multiple-gpus) for
the details.

HDR
---

The modesetting driver can run monitors that support HDR10 in HDR mode.
On a monitor without HDR, or with HDR turned off, HDR windows are tone mapped to SDR.

Without compositing (see above) only fullscreen HDR windows are
supported. The gamma ramp (`xrandr --gamma`, night light tools) isn't applied
to HDR monitors that are composited in HDR10, and rotated or scaled HDR
monitors tone map HDR windows instead of showing them as HDR.

On AMD GPUs with Linux older than 6.19 the screen can flicker briefly on an
HDR monitor when a fullscreen window that isn't HDR starts or stops being shown
directly, for example when a video player enters fullscreen.

HDR support works without the "Depth 30" config option set (in fact, you
shouldn't set it), so programs function and look correctly.

HDR needs:

- A monitor that supports HDR
- the modesetting driver
- a kernel driver that exposes the DEGAMMA_LUT, CTM and GAMMA_LUT CRTC
  properties and the HDR_OUTPUT_METADATA connector property (tested with
  amdgpu)

To enable HDR on all monitors when the X server starts, set the options in
the Device section of xorg.conf, for example in
/etc/X11/xorg.conf.d/20-modesetting.conf:

    Section "Device"
        Identifier "GPU"
        Driver     "modesetting"
        Option     "HDR"           "true"
        Option     "SDRWhiteLevel" "203"
    EndSection

`SDRWhiteLevel` is the brightness in nits that SDR content, such as the
desktop, is shown at in HDR mode. The default is 203.

These options only set the starting values. Each output has RandR properties
that can be changed while the X server runs:

- `HDR`: 1 enables HDR on the output, 0 disables it.
- `SDR white level`: the SDR white level in nits, from 1 to 10000.
- `HDR capable`: read-only, 1 if the monitor supports HDR10.

For example:

    xrandr --prop
    xrandr --output DP-1 --set HDR 1
    xrandr --output DP-1 --set "SDR white level" 203

Changes made with xrandr last until the X server exits.

For best result change your monitor/TVs "Dynamic Tone Mapping" option to "HGiG" or "Off".

Vulkan games and applications can output HDR through the X11 HDR Vulkan layer
[VK_hdr_layer](https://github.com/dec05eba/VK_hdr_layer), and mpv through an
[mpv build with X11 HDR support](https://github.com/dec05eba/mpv).
An application that does this itself sets the `hdr_output_metadata` window
property (type CARDINAL, format 8) to a `struct hdr_output_metadata` from the
kernel's drm_mode.h. It renders PQ content with BT.2020 primaries into a
window with a 30-bit visual.

Here's a demo that shows HDR working in this X server:

[![HDR demo video](https://img.youtube.com/vi/Hb-kc35BnuM/hqdefault.jpg)](https://www.youtube.com/watch?v=Hb-kc35BnuM "HDR on Linux with X11")

DPI
---

Every output has a `DPI` RandR property that tells applications which scale to
use on that monitor, where 96 means 1.0 scaling and 192 means 2.0 scaling. It
starts at the value of the `-dpi` X server option, or 96 if that isn't set, and
can be changed while the X server runs:

    xrandr --output DP-1 --set DPI 192

The X server doesn't use the value itself, it's up to applications to read it.
Applications should prefer this `DPI` RandR property and only fall back to
other properties, such as `XResources` `Xft.dpi`, if the RandR property isn't
set. Applications should also use the DPI of the monitor that the center of
the window is inside and when changing scale the size of the window itself
shouldn't change, only the content should scale.

NVIDIA
------

The modesetting driver also works with the proprietary NVIDIA driver, instead
of NVIDIA's own Xorg driver, so compositing, HDR and the other
features above can be used on NVIDIA GPUs.

It needs kernel modesetting enabled in the NVIDIA kernel driver
(`nvidia-drm.modeset=1`) and an NVIDIA driver with GBM support (495 or newer).
Select the modesetting driver in the Device section of xorg.conf (for example `/etc/X11/xorg.conf.d/10-modesetting.conf`):

    Section "Device"
        Identifier "GPU"
        Driver     "modesetting"
    EndSection

HDR also needs the NVIDIA kernel driver to expose the properties listed in the
HDR section.

Multiple GPUs
-------------

Monitors connected to a second GPU (reverse PRIME), such as the HDMI port that
is wired to the NVIDIA GPU on many laptops, are tear-free and support HDR when
compositing is on.

When that GPU is too slow to copy frames to such a monitor in time, for example
because it saves power while it has little else to do, the monitor shows
frames one refresh later with triple buffering instead of dropping them. It
switches back once the copies are fast again.

Fullscreen applications that render on the GPU their monitor is connected to,
for example a game started with `prime-run` on that HDMI monitor, are handled
by that GPU alone. The other GPU doesn't composite or copy their frames, so
there is practically no performance penalty compared to a system that doesn't
use PRIME (a system with only one GPU).

Applications that render on the other GPU draw into linear buffers while they
are fullscreen on such a monitor, so the GPU the monitor is connected to can
copy them to the screen without the window being composited.