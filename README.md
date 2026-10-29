X Server
--------

The X server accepts requests from client applications to create windows,
which are (normally rectangular) "virtual screens" that the client program
can draw into.

Windows are then composed on the actual screen by the X server
(or by a separate composite manager) as directed by the window manager,
which usually communicates with the user via graphical controls such as buttons
and draggable titlebars and borders.

For a comprehensive overview of X Server and X Window System, consult the
following article:
https://en.wikipedia.org/wiki/X_server

All questions regarding this software should be directed at the
Xorg mailing list:

  https://lists.freedesktop.org/mailman/listinfo/xorg

The primary development code repository can be found at:

  https://gitlab.freedesktop.org/xorg/xserver

For patch submission instructions, see:

  https://www.x.org/wiki/Development/Documentation/SubmittingPatches

As with other projects hosted on freedesktop.org, X.Org follows its
Code of Conduct, based on the Contributor Covenant. Please conduct
yourself in a respectful and civilized manner when using the above
mailing lists, bug trackers, etc:

  https://www.freedesktop.org/wiki/CodeOfConduct

Per-monitor framebuffers
------------------------

With the modesetting driver every monitor has its own framebuffer.
There is no framebuffer that covers all monitors. Every toplevel window
has its own pixmap and the X server composites the visible windows into
each monitor's framebuffer on the GPU, only where something changed,
and flips it in sync with the monitor so there's no tearing.

A fullscreen window on any monitor is shown without copies: when it presents
with Present (Vulkan and OpenGL do) its buffer is put on the monitor directly.
This also works for a window inside a window manager frame. HDR and variable
refresh rate follow the window that is shown this way on each monitor.

In this fork the Xorg server now also does the compositing work, so other compositors aren't
needed and that also allows for lower latency while compositing.
When you have a fullscreen window the compositing is bypassed, for maximum performance.

- Windows with an alpha channel are transparent and `_NET_WM_WINDOW_OPACITY`
  is supported, so you don't need a compositor such as picom.
- The X server owns the `_NET_WM_CM_S<screen>` selection, so picom and similar
  compositors refuse to start. Window managers with a built-in compositor,
  such as KWin and xfwm4, run with their compositor disabled. Mutter based
  window managers always composite and may refuse to start unless they are
  started with `--replace`. A compositor that takes over the selection still
  works: the X server shows its overlay window.

This is enabled when the modesetting driver uses glamor. Set
`Option "TearFree" "false"` in the Device section to go back to one shared
framebuffer for all monitors.

Limitations:

- Drawing directly on the root window, other than its background, isn't
  shown. Desktop wallpapers set with tools like `xsetroot` and `feh` work.
- Reading the root window works with GetImage (screenshots, `xwd`, ffmpeg
  x11grab), but not with CopyArea or Render.
- Monitors on a secondary GPU (reverse PRIME) are composited into a buffer
  shared with that GPU, so fullscreen windows on them are copied instead of
  scanned out directly.
- A software cursor is drawn by the X server and stops direct scanout on the
  monitor it is on.

HDR
---

The modesetting driver can run monitors that support HDR10 in HDR mode. The
desktop is converted to HDR10 by the GPU, and a fullscreen window that sets
the `hdr_output_metadata` property is shown as HDR without conversion.
Switching between the two doesn't change the display mode, so it's instant.
Turning HDR on or off for a monitor does change the mode, so the monitor goes
black for a moment. HDR is off by default.

HDR windows also work when they aren't fullscreen. While one is visible on an
HDR monitor, the X server composites that monitor in HDR10 and shows the parts
of the window that nothing covers without conversion.

On a monitor without HDR, or with HDR turned off, HDR windows are tone mapped
to SDR. A fullscreen HDR window is still shown directly there, and the tone
mapping is done by the monitor's color pipeline (DEGAMMA_LUT, CTM and
GAMMA_LUT) instead of by copying.

Without per-monitor framebuffers (see above) only fullscreen HDR windows are
supported. The gamma ramp (`xrandr --gamma`, night light tools) isn't applied
to a monitor while it composites in HDR, and rotated or scaled HDR monitors
tone map HDR windows instead of showing them as HDR.

HDR support works without the "Depth 30" config option set (in fact, you shouldn't set it),
so programs function and look correct.

HDR needs:

- the modesetting driver
- a kernel driver that exposes the DEGAMMA_LUT, CTM and GAMMA_LUT CRTC
  properties and the HDR_OUTPUT_METADATA connector property (tested with
  amdgpu)
- a monitor whose EDID reports HDR10 support

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

For best result change your monitor/TVs "Dynamic Tone Mapping" option to "HGiG" (or "On" if it's not available).

Vulkan games and applications can output HDR through the X11 HDR Vulkan layer
[VK_hdr_layer](https://github.com/dec05eba/VK_hdr_layer), and mpv through an [mpv build with X11 HDR support](https://github.com/dec05eba/mpv).
An application that does this itself sets the `hdr_output_metadata` window
property (type CARDINAL, format 8) to a `struct hdr_output_metadata` from the
kernel's drm_mode.h. It renders PQ content with BT.2020 primaries into a
window with a 30-bit visual.

Note that when installing this latest Xorg server version you also need to rebuild and install the latest versions of Xorg drivers from source, such as [xf86-input-libinput](https://github.com/dec05eba/xf86-input-libinput).

DPI
---

Every output has a `DPI` RandR property that tells applications which scale to
use on that monitor, where 96 means 1.0 scaling and 192 means 2.0 scaling. It
starts at the value of the `-dpi` X server option, or 96 if that isn't set, and
can be changed while the X server runs:

    xrandr --output DP-1 --set DPI 192

The X server doesn't use the value itself, it's up to applications to read it.
Applications should prefer this `DPI` RandR property and only fallback to other properties, such as `XResources` `Xft.dpi` it the RandR property isn't set.
Applications should also use the DPI of the monitor that the center of the window is inside and when changing scale the size of the window itself shouldn't change, only the content should scale.

NVIDIA
------

The modesetting driver also works with the proprietary NVIDIA driver, instead
of NVIDIA's own Xorg driver, so per-monitor framebuffers, HDR and the other
features above can be used on NVIDIA GPUs. This is untested.

It needs kernel modesetting enabled in the NVIDIA kernel driver
(`nvidia-drm.modeset=1`) and an NVIDIA driver with GBM support (495 or newer).
Select the modesetting driver in the Device section of xorg.conf:

    Section "Device"
        Identifier "GPU"
        Driver     "modesetting"
    EndSection

HDR also needs the NVIDIA kernel driver to expose the properties listed in the
HDR section.
