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
  don't bypass compositing.
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

HDR
---

The modesetting driver can run monitors that support HDR10 in HDR mode.
On a monitor without HDR, or with HDR turned off, HDR windows are tone mapped to SDR.

Without compositing (see above) only fullscreen HDR windows are
supported. The gamma ramp (`xrandr --gamma`, night light tools) isn't applied
to HDR monitors that are composited in HDR10, and rotated or scaled HDR
monitors tone map HDR windows instead of showing them as HDR.

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
    xrandr --output DP-1 --set "SDR white level" 250

Changes made with xrandr last until the X server exits.

For best result change your monitor/TVs "Dynamic Tone Mapping" option to "HGiG" or "Off".

Vulkan games and applications can output HDR through the X11 HDR Vulkan layer
[VK_hdr_layer](https://github.com/dec05eba/VK_hdr_layer), and mpv through an
[mpv build with X11 HDR support](https://github.com/dec05eba/mpv).
An application that does this itself sets the `hdr_output_metadata` window
property (type CARDINAL, format 8) to a `struct hdr_output_metadata` from the
kernel's drm_mode.h. It renders PQ content with BT.2020 primaries into a
window with a 30-bit visual.

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
Select the modesetting driver in the Device section of xorg.conf:

    Section "Device"
        Identifier "GPU"
        Driver     "modesetting"
    EndSection

HDR also needs the NVIDIA kernel driver to expose the properties listed in the
HDR section.
