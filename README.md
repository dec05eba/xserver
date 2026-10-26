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

HDR
---

The modesetting driver can run monitors that support HDR10 in HDR mode. The
desktop is converted to HDR10 by the GPU, and a fullscreen window that sets
the `hdr_output_metadata` property is shown as HDR without conversion.
Switching between the two doesn't change the display mode, so it's instant.
Turning HDR on or off for a monitor does change the mode, so the monitor goes
black for a moment. HDR is off by default.

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

You also have to turn off your compositor to make HDR display at the moment
and for proper multimonitor support.

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
[VK_hdr_layer](https://git.dec05eba.com/VK_hdr_layer/), and mpv through an [mpv build with X11 HDR support](https://git.dec05eba.com/mpv/).
An application that does this itself sets the `hdr_output_metadata` window
property (type CARDINAL, format 8) to a `struct hdr_output_metadata` from the
kernel's drm_mode.h. It renders PQ content with BT.2020 primaries into a
window with a 30-bit visual. The property is only used while the window covers
the monitor and HDR is enabled on it.
