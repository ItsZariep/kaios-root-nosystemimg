# X.org on KaiOS?

> To follow this, i must assume you got real root.

Idea: KaiOS is just android-like, which is just Linux, Right? why can't we just kill b2g and run standard software? it is unfair!

so in theory, we just need to open devices and startx? right? let's see

# Devices

Target devices tested against: GQWERTY (KaiOS, MT6572, kernel 3.4.67) (this didn't work, see below) and a Gflip 3 T-Mobile (MSM8909, kernel 3.10.49, dual-screen flip phone with ST7789H2 SPI cover panel).

Goal: keep the vendor kernel and drivers, replace only the userland, and get a usable X11 (or at least framebuffer) session running alongside/instead of the stock UI (b2g / Gonk).

## Prerequisites

- `adb` and `real root` access to the device.
- A static ARM cross-compiler. **musl-based** toolchains work best here, since Bionic's modern static libc calls syscalls (e.g. `MADV_WIPEONFORK`) that don't exist on these old 3.x kernels and will abort at startup. Recommended: `musl.cc` prebuilt cross toolchains, e.g. `armv7l-linux-musleabihf-cross.tgz` used before.
- Full backup of all partitions before doing anything irreversible (especially before any bootloader-level root method).

## Get an Alpine Linux minimal rootfs

- Use **Alpine 3.12** (armv7/armhf), not a newer release. Alpine versions from roughly 3.15+ require `clock_gettime64`/time64 syscalls that don't exist on pre-4.x kernels, and will fail at runtime.
- Download the `alpine-minirootfs-3.12.x-armv7.tar.gz` tarball (not the ISO/virt images, which aren't meant for chroot use).
- Push to the device: `adb push alpine-minirootfs.tar.gz /sdcard/`.
- As root on-device, extract directly into a writable location (no loop-mount needed once you have root and can write anywhere):

```
adb push alpine-minirootfs-3.12.0-armv7.tar.gz /sdcard/ 
```

```sh
mkdir -p /data/local/tmp/mnt/alpine
tar -xzf /sdcard/alpine-minirootfs-3.12.0-armv7.tar.gz -C /data/local/tmp/mnt/alpine
```

## Prepare the chroot environment

```sh
mount -o bind /dev /data/local/tmp/mnt/alpine/dev
mount -t proc proc /data/local/tmp/mnt/alpine/proc
mount -t sysfs sys /data/local/tmp/mnt/alpine/sys
mount -t devpts devpts /data/local/tmp/mnt/alpine/dev/pts
ln -sf /dev/graphics/fb0 /data/local/tmp/mnt/alpine/dev/fb0   # KaiOS/Android puts fb at /dev/graphics/fb0, not /dev/fb0

```

Check `getenforce` / `cat /sys/fs/selinux/enforce`:

- if `Enforcing` despite the kernel cmdline saying otherwise (vendor `init.rc` often re-enables it mid-boot)
- run `setenforce 0` with root. In practice this often turns out to be a red herring (see step 7), don't assume every "Operation not permitted" is SELinux.


## Enter the chroot and set up networking

```sh
./busybox chroot /data/local/tmp/mnt/alpine /bin/sh
export PATH=/bin:/usr/bin:/sbin:/usr/sbin   # chroot doesn't set this for you: Android's PATH has none of Alpine's binaries
export HOME=/root
echo "nameserver 8.8.8.8" > /etc/resolv.conf
ping -c1 8.8.8.8  # confirm the chroot can reach the network (it shares the host's netns)
apk update
```

If `apk update` 404s, Alpine 3.12 may have aged out of the main CDN mirror, check `/etc/apk/repositories` and switch to the archive mirror if needed.

Install the X stack and basic tools:

```sh
apk add xorg-server xf86-video-fbdev xf86-input-evdev xterm xkeyboard-config setxkbmap evtest
```

## Stop the stock UI (B2G)

```sh
setprop ctl.stop b2g  # or whatever the main UI service is called on this device — check `getprop | grep init.svc`
```

Leave modem-related services alone (`rild`, `ccci_mdinit`, `netd`, etc.) unless you specifically want to lose connectivity too.

## The framebuffer "black screen" problem (the hard part)

Symptoms encountered: Xorg starts cleanly (banner, no fatal errors), but the physical screen stays black, even though the backlight can be controlled fine via `/sys/class/leds/lcd-backlight/brightness` (and, for dual-screen devices, `/sys/class/leds/subbacklight/brightness` for a secondary/cover panel).

Root cause on this hardware (MDSS/MDP3 driving an SPI command-mode panel, e.g. ST7789H2): this is **not** a simple video-mode framebuffer where "whatever's in memory just shows up." It needs:

1. Writes to go through **`mmap()`**, not a plain `write()`/`cat`, on this driver, raw `write()` may silently succeed without ever reaching real video memory.
2. An explicit **`FBIOPAN_DISPLAY`** ioctl call after writing, to commit/latch the frame to the panel (command-mode panels have their own internal RAM and don't refresh from host memory continuously on their own).
3. The file descriptor used for the mmap must **stay open**: on this driver, `fb_release()` (triggered by `close()`) blanks the panel again. A short-lived test program that writes, pans, and exits will show a correct frame for an instant and then go black again.
4. Watch out for `line_length`/`smem_len` padding: the real stride can be wider than `xres * bytes_per_pixel` (e.g. this panel's actual `line_length` was 1024 bytes for a nominal 240px-wide/16bpp row, not 480). Always get this from `FBIOGET_FSCREENINFO`, never assume it.
5. The fb device may reject a second concurrent `open()` while another process holds it (Xorg itself, or a competing test tool): errors here can present as either `EPERM` or `ENODEV` depending on what's contending.

**`fbtest.c`** is the diagnostic tool built around these findings: it opens `/dev/graphics/fb0`, reads `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` (to get real dimensions/stride), `mmap()`s the real video memory, writes a test pattern, calls `FBIOPAN_DISPLAY`, and (in its final form) holds the fd open indefinitelyinstead of exiting, to prove the "must stay open" theory. This confirmed the mechanism works: the screen lit up and stayed lit as long as the process was alive, and went black the instant it exited.

## Getting Xorg itself to behave the same way

Problem: `xf86-video-fbdev`'s own internal handling of this panel doesn't keep the fd open/re-pan correctly on its own (likely closes/reopens around VT switches, or doesn't call `FBIOPAN_DISPLAY` after every shadow-framebuffer flush for this particular driver), so Xorg alone reproduced the same "flash then black" symptom.

Solution used: **`LD_PRELOAD` shim**, to test the fix without touching/recompiling Xorg or the fbdev X driver source.

**`fbhack.c` / `fbhack.so`**: a small shared library that:
- Intercepts `open()`, detects when the path is `/dev/graphics/fb0`, and records the fd.
- Spawns a background thread that periodically calls `FBIOGET_VSCREENINFO` + `FBIOPAN_DISPLAY` on that fd (e.g. every 100ms).
- Intercepts `close()` and, if called on the tracked fb fd, **no-ops it** (returns success without actually closing), to prevent the driver's blank-on-release behavior from kicking in.

Build:

```sh
armv7l-linux-musleabihf-gcc -shared -fPIC -O2 -o fbhack.so fbhack.c -ldl -lpthread
adb push fbhack.so /data/local/tmp/
```
then copy to chroot.

Use:

```sh
export LD_PRELOAD=/lib/fbhack.so
Xorg :0 -novtswitch -sharevts -keeptty -config /etc/X11/xorg.conf &
```

This worked: Xorg's actual rendered output became visible and stayed visible.

(Longer-term, a cleaner fix would be patching `xf86-video-fbdev` directly, not closing the fb fd on cleanup, and calling `FBIOPAN_DISPLAY` after every flush,  but the `LD_PRELOAD` shim is a fully sufficient and much faster way to validate the approach works before investing in a source patch, and is fine to keep using as-is.)

## Minimal working `xorg.conf`

Must be a **complete**, self-consistent file once you add any explicit section (e.g. `ServerLayout`), Xorg stops auto-generating the implicit defaults it otherwise uses, so a partial/appended config can break with "no screens found" even if it worked before you added more sections.

```
Section "Device"
    Identifier "fbdev"
    Driver "fbdev"
    Option "fbdev" "/dev/graphics/fb0"
    Option "ShadowFB" "false"
EndSection

Section "Monitor"
    Identifier "Monitor0"
EndSection

Section "Screen"
    Identifier "Default Screen Section"
    Device "fbdev"
    Monitor "Monitor0"
EndSection

Section "InputDevice"
    Identifier "Keypad"
    Driver "evdev"
    Option "Device" "/dev/input/event0"
EndSection

Section "ServerLayout"
    Identifier "Default Layout"
    Screen "Default Screen Section"
    InputDevice "Keypad" "CoreKeyboard"
EndSection
```

Notes:
- Find the correct keypad `eventN` via `cat /proc/bus/input/devices` (look for a name like `matrix_keypad`, not `qpnp_pon`/power-button or ALSA jack devices that also show up as input nodes).
- Don't declare a `CorePointer` / touch `InputDevice`, Xorg 1.20 tolerates a keyboard-only layout fine.
- If keys reach X but don't produce usable characters, check `xkeyboard-config`/`setxkbmap` are installed, and try `Option "XkbLayout" "us"` explicitly in the `InputDevice` section.

## Starting a session end to end

```sh
# as root, outside chroot:
setprop ctl.stop b2g
chroot /data/local/tmp/mnt/alpine /bin/sh
export PATH=/bin:/usr/bin:/sbin:/usr/sbin
export LD_PRELOAD=/lib/fbhack.so
Xorg :0 -novtswitch -sharevts -keeptty -config /etc/X11/xorg.conf &
DISPLAY=:0 xterm &
```

## No available TTYs?

This happened to me on the GQWERTY, so maybe SDL2 apps work? not deeply tested since is kinda pointless

## Open items / next steps

- make keypad input actually usable in xterm with the corrected `xorg.conf` (previous failures were due to an incomplete/inconsistent config file, not an input driver problem raw `event0` data was confirmed working at the kernel level via `evtest`/hexdump).