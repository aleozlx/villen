# Steam Deck debugging guide

Hard-won notes for building, deploying, and testing a native SDL2 + OpenGL +
Dear ImGui binary (the Villen host, and the step-6 spike) on a Steam Deck. Every
item here is something that actually bit us; most are *environment* issues, not
code bugs, which is exactly why DESIGN §11 front-loads a throwaway spike to flush
them out before the real admin UI exists.

> Conventions: `<deck-ip>` is the Deck's LAN address; `<ssh-account>` is whatever
> account you SSH in as. Commands assume a Linux PC as the build host.

---

## 1. What the Deck actually is

- **SteamOS, x86-64**, with an **immutable / read-only root filesystem**
  (`steamos-readonly status` → `enabled`). You cannot `pacman -S` build deps
  without disabling that, and changes are wiped on OS updates. Treat the rootfs
  as fixed.
- **No build toolchain.** No `gcc`/`g++`/`cmake`/`ninja`/`make`/`pkg-config`.
  **Building *on* the Deck is not an option — cross-build on a PC and copy.**
- **Runtime libraries are present**: `libSDL2-2.0.so.0`, GLVND
  (`libGLX`/`libOpenGL`/`libGL.so.1`), `libgbm`, X11/xcb. So a dynamically linked
  SDL2+GL binary built elsewhere will *load* — provided the glibc check below.
- **Two-session reality.** Steam (in **both** Desktop and Game Mode) runs as the
  stock **`deck`** user. If you SSH in as a *different* account, remember that
  `deck`'s home is mode `0700` — see §4.
- **Game Mode = Gamescope.** In Game Mode the Deck runs the Gamescope compositor;
  native apps launch as **non-Steam shortcuts** and render as XWayland clients
  (SDL picks the **x11** video driver).

---

## 2. The glibc trap (this one looks like a mystery crash)

**Symptom:** the app "crash-quits" instantly in Game Mode — window never appears,
no output, no log.

**Cause:** the binary was built on a PC whose glibc is **newer** than the Deck's.
A bleeding-edge distro binds the newest symbol versions; e.g. glibc 2.43
re-versioned some long-standing single-precision math functions, so the binary
imports `sqrtf@GLIBC_2.43` / `acosf@GLIBC_2.43` / `atan2f@GLIBC_2.43` (ImGui's
geometry code pulls these in). The Deck (glibc **2.41** at time of writing) does
not export those version nodes, so the **dynamic loader aborts before `main()`**.

**Diagnose** — this is the single most useful command; run it on the PC before
copying, and on the Deck if already deployed:

```bash
# what the binary needs:
objdump -T ./your_binary | grep -oE 'GLIBC_[0-9.]+' | sort -V -u | tail
# on the Deck, what's missing:
ldd ./your_binary            # prints "version `GLIBC_2.xx' not found"
```

**Fix, cheapest first:**
1. **Static-link the C++ runtime** so libstdc++ ABI can't mismatch:
   `-static-libstdc++ -static-libgcc`. (Does *not* fix libc/libm — see 2 & 3.)
2. **Pin the offending libc symbols** back to an old version node that exists on
   both glibcs, via a force-included `.symver` shim (`spike/deck/glibc_compat.h`):
   ```c
   __asm__(".symver sqrtf,  sqrtf@GLIBC_2.2.5");
   __asm__(".symver acosf,  acosf@GLIBC_2.2.5");
   __asm__(".symver atan2f, atan2f@GLIBC_2.2.5");
   ```
   force-included with `-include glibc_compat.h` on every TU. Confirm the old
   nodes exist: `objdump -T /usr/lib/libm.so.6 | grep -wE 'sqrtf|acosf|atan2f'`
   on *both* machines. After rebuilding, the `objdump` check above should show
   nothing newer than the Deck's glibc.
3. **Most robust:** build inside a container whose glibc is **≤** the Deck's
   (e.g. an older Debian/Ubuntu or the Steam Linux Runtime). Then you don't play
   symbol-version whack-a-mole at all.

---

## 3. Getting files onto the Deck

- **SSH** is a normal `sshd`; enable it once in Desktop Mode if needed
  (`passwd` to set a password, `sudo systemctl enable --now sshd`). It then works
  from either mode.
- **Account/home mismatch.** A binary launched from a Steam non-Steam shortcut
  runs as **`deck`**, so it must live somewhere `deck` can read+execute. If you
  SSH as another account whose home is `0700`, `deck` can't reach files there.
  Options:
  - copy via a world-readable drop: put the file in `/tmp/...` (mode `0755`) as
    your SSH account, then from a `deck` shell `cp` it into `~/` (i.e.
    `/home/deck/...`); or
  - authorize your key for `deck` directly (append to
    `/home/deck/.ssh/authorized_keys`, `chmod 700 ~/.ssh`, `chmod 600` the file)
    and deploy straight into `/home/deck`.

### 3.1 Long jobs over SSH die on disconnect (the logind trap)

**Symptom:** a model download (or any long job) started over SSH stops the moment
the `ssh` command returns — even with `nohup` *and* `setsid`. You reconnect and the
file is half-written, the process gone.

**Cause:** SteamOS's `systemd-logind` tears down the **whole user slice** when your
last session for that user ends (`KillUserProcesses` behaviour). `nohup` (ignores
SIGHUP) and `setsid` (new session) don't help — logind kills the cgroup, not via
SIGHUP, so detaching from the terminal doesn't exempt you.

**Fix — keep a session alive for the job's duration.** Simplest is to run the job
in the **foreground of a long-lived `ssh`** (e.g. a backgrounded task on the build
host that holds the connection); when it returns, the job is done. Alternatives:
`systemd-run --scope -- <cmd>` (runs in a transient scope that outlives the
session) or `loginctl enable-linger deck` once (keeps the user manager running).

**Downloading weights on the Deck.** The 4–5 GB GGUFs come off HuggingFace, whose
CDN drops connections mid-transfer; a plain `curl -C -` won't resume a *broken*
stream, it just exits non-zero. Wrap it in a resume-until-complete loop, inside the
held session:

```bash
until curl -L -C - --retry 20 --retry-delay 3 --retry-all-errors \
           --connect-timeout 20 -o "$out" "$url"; do sleep 3; done
```

(`-C -` resumes from bytes-on-disk on each restart; `--retry` rides out transient
errors within one curl run.) Put weights on the SD card (`/run/media/deck/SD256/…`)
— roomy, but slow, and the load cost is sneaky: a **cold** model load is I/O-bound
off the card, and llama.cpp's default **mmap does random reads**, which the card
handles badly — a cold 7B Q4 measured **~10 min**, vs ~1 min warm (and a concurrent
download, or loading another model that evicts this one from page cache, makes a
"fast" load suddenly cold again — that's the usual surprise). Mitigations: pre-read
the file sequentially to warm the cache (`cat model.gguf > /dev/null`, ~1 min at the
card's sequential rate) so the mmap load hits RAM, or pass `llama-server --no-mmap`
(sequential load, more RSS). A warm load is then seconds.

### 3.2 Redeploying a *running* binary

- **`scp` fails with `dest open … : Failure` (ETXTBSY).** You can't overwrite an
  executable that's currently running. Stop the process first, then copy.
- **`pkill -f "Villen/villen"` kills its own shell.** With `-f` (full-cmdline
  match) the pattern string appears in pkill's *own* argv, so it matches and kills
  the shell running it before reaching the target — looks like "the kill silently
  did nothing." Match on the process name instead: `pkill -x villen` (and
  `pkill -x llama-server`); the killing shell's `comm` is `bash`/`ssh`, not the
  target, so no self-match. Verify with `pgrep -xc villen` before re-`scp`.

---

## 4. Game Mode testing (the risks the spike exists to retire)

Add the binary as **Add a Non-Steam Game → Browse**, set **Start In** to its
folder (so any logfile lands there), then **Switch to Game Mode** and launch it.
For the matching custom library capsule, hero, header, and logo, see
[`villen-art-brief.md` §6](villen-art-brief.md#6-use-the-art-in-the-steam-library-non-steam-shortcut).

| Symptom | Cause | Fix |
|---|---|---|
| Window doesn't appear / instant quit | Usually the glibc trap (§2) | `ldd`/`objdump` check, then symver pin |
| `controllers: 0` or D-pad does nothing | Steam Input is remapping the pad to keyboard/mouse | Steam button → **Controller Settings** → set the **"Gamepad"** template (no-remap). SDL then sees a `Steam Virtual Gamepad`. |
| Buttons reach SDL but ImGui won't navigate | `ImGuiConfigFlags_NavEnableGamepad` not set | enable it; verify `io.NavActive` flips to `yes` |
| Black screen / GL init fails | GL context/version | request GL 3.0+, GLSL `#version 130`; the Deck reports GL **4.6** `radeonsi` (`vangogh` APU) — if you instead see `llvmpipe`, you're on software GL, not the real driver |
| Window not fullscreen | window flags | `SDL_WINDOW_FULLSCREEN_DESKTOP`; Gamescope composites it across the panel |

**Self-diagnostics.** Have the app print what it sees (SDL video driver, `GL_VERSION`,
`GL_RENDERER`, controller count/names) **on-screen** (Game Mode has no terminal)
**and** to a logfile you can read over SSH. The step-6 spike (`spike/deck/`) does
exactly this; a healthy run logs:

```
SDL video driver: x11
controller[0]: Steam Virtual Gamepad
GL_RENDERER: AMD Custom GPU 0405 (radeonsi, vangogh, ...)
window + GL + imgui up; entering loop
```

**Screenshots.** In Game Mode, **Steam + R1** captures. Files land under
`~/.local/share/Steam/userdata/<id>/760/remote/<appid>/screenshots/` (full-res
`.jpg`, plus `thumbnails/`); pull the newest over SSH.

---

## 5. LAN reachability (remote browser players)

- The server binds `0.0.0.0`, so it's reachable on the LAN; it prints its
  `http://<deck-ip>:<port>` URLs on startup. Open that on a phone/laptop on the
  **same WiFi**.
- **Watch for AP client isolation.** Many shared/guest/public APs silently block
  device-to-device traffic, so the phone can't reach the Deck even though both
  have internet. That's a *network* property, not a code bug — test on a network
  you control, or check the AP's "client isolation / AP isolation" setting.
- Quick server-side check while a client should be connected:
  `ss -tn | grep <port>` shows established peers; `ss -ltn | grep <port>`
  confirms it's listening on `0.0.0.0`.

---

## 6. TL;DR checklist

1. Cross-build on PC (`-static-libstdc++ -static-libgcc`).
2. `objdump -T bin | grep GLIBC_` ≤ the Deck's glibc — symver-pin anything newer.
3. Deploy into a path the **`deck`** user can read/execute.
4. Non-Steam shortcut → **Gamepad** controller template → Game Mode.
5. Confirm on-screen: real `radeonsi` renderer, ≥1 controller, D-pad navigates.
6. Phone on the same (non-isolating) WiFi hits `http://<deck-ip>:<port>`.
