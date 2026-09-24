#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: patch_stage435_event_transport.py INPUT OUTPUT")

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding="utf-8")

def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0:
        raise SystemExit("function not found: " + signature)
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit("opening brace not found: " + signature)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + replacement.rstrip() + text[i+1:]
    raise SystemExit("unterminated function: " + signature)

def replace_struct(text, name, replacement):
    signature = "struct " + name
    start = text.find(signature)
    if start < 0:
        raise SystemExit("struct not found: " + name)
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit("struct opening brace not found: " + name)
    depth = 0
    end = None
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                semi = text.find(";", i)
                if semi < 0:
                    raise SystemExit("struct semicolon missing: " + name)
                end = semi + 1
                break
    if end is None:
        raise SystemExit("unterminated struct: " + name)
    return text[:start] + replacement.rstrip() + text[end:]

gamepad_struct = r'''struct GamepadInput {
   int fd = -1;
   std::string path;
   std::string name;
   bool buttons[H3531_JS_MAX_BUTTONS]{};
   int16_t axes[H3531_JS_MAX_AXES]{};
   int axis_zone[H3531_JS_MAX_AXES]{};
   uint8_t key_to_button[KEY_MAX + 1]{};
   uint8_t abs_to_axis[ABS_MAX + 1]{};
   input_absinfo absinfo[H3531_JS_MAX_AXES]{};
   GamepadProfile profile;
};'''

input_struct = r'''struct Input {
   int fd = -1;                 // keyboard evdev
   std::string path;
   GamepadInput pads[H3531_MAX_GAMEPADS];
   int watch_fd = -1;
   int watch_wd = -1;
   bool rescan_requested = true;
   uint64_t next_fallback_scan_ms = 0;
};'''

src = replace_struct(src, "GamepadInput", gamepad_struct)
src = replace_struct(src, "Input", input_struct)

transport = r'''
static const unsigned H3531_BITS_PER_LONG_FRONT =
      8U * (unsigned)sizeof(unsigned long);
static const unsigned H3531_EVENT_MAX_FRONT = 64U;

static bool h3531_front_test_bit(unsigned bit, const unsigned long *bits)
{
   return ((bits[bit / H3531_BITS_PER_LONG_FRONT] >>
         (bit % H3531_BITS_PER_LONG_FRONT)) & 1UL) != 0;
}

static int16_t h3531_front_scale_abs(const input_absinfo &info, int value)
{
   const long long range =
      (long long)info.maximum - (long long)info.minimum;
   if (range <= 0) return 0;

   long long scaled =
      ((long long)value - (long long)info.minimum) * 65534LL / range;
   scaled -= 32767LL;
   if (scaled > 32767LL) scaled = 32767LL;
   if (scaled < -32767LL) scaled = -32767LL;
   return (int16_t)scaled;
}

static bool input_event_gamepad_capable(int fd)
{
   const size_t ev_words =
      (EV_MAX + H3531_BITS_PER_LONG_FRONT) / H3531_BITS_PER_LONG_FRONT;
   const size_t key_words =
      (KEY_MAX + H3531_BITS_PER_LONG_FRONT) / H3531_BITS_PER_LONG_FRONT;
   const size_t abs_words =
      (ABS_MAX + H3531_BITS_PER_LONG_FRONT) / H3531_BITS_PER_LONG_FRONT;

   std::vector<unsigned long> evbits(ev_words, 0);
   std::vector<unsigned long> keybits(key_words, 0);
   std::vector<unsigned long> absbits(abs_words, 0);

   if (ioctl(fd, EVIOCGBIT(0, evbits.size() * sizeof(unsigned long)),
         evbits.data()) < 0)
      return false;
   if (!h3531_front_test_bit(EV_KEY, evbits.data()))
      return false;
   if (ioctl(fd, EVIOCGBIT(EV_KEY, keybits.size() * sizeof(unsigned long)),
         keybits.data()) < 0)
      return false;

   unsigned game_keys = 0;
   unsigned axes = 0;

   for (int code = BTN_JOYSTICK; code < KEY_MAX; ++code)
      if (h3531_front_test_bit((unsigned)code, keybits.data()))
         ++game_keys;

   if (h3531_front_test_bit(KEY_UP, keybits.data())) ++game_keys;
   if (h3531_front_test_bit(KEY_DOWN, keybits.data())) ++game_keys;
   if (h3531_front_test_bit(KEY_LEFT, keybits.data())) ++game_keys;
   if (h3531_front_test_bit(KEY_RIGHT, keybits.data())) ++game_keys;

   if (h3531_front_test_bit(EV_ABS, evbits.data()) &&
       ioctl(fd, EVIOCGBIT(EV_ABS, absbits.size() * sizeof(unsigned long)),
         absbits.data()) >= 0)
      for (int code = 0; code <= ABS_MAX; ++code)
         if (h3531_front_test_bit((unsigned)code, absbits.data()))
            ++axes;

   return game_keys >= 4 || (game_keys >= 2 && axes >= 2);
}

static void input_build_event_maps(GamepadInput &pad, int fd)
{
   const size_t key_words =
      (KEY_MAX + H3531_BITS_PER_LONG_FRONT) / H3531_BITS_PER_LONG_FRONT;
   const size_t abs_words =
      (ABS_MAX + H3531_BITS_PER_LONG_FRONT) / H3531_BITS_PER_LONG_FRONT;
   std::vector<unsigned long> keybits(key_words, 0);
   std::vector<unsigned long> absbits(abs_words, 0);

   ioctl(fd, EVIOCGBIT(EV_KEY, keybits.size() * sizeof(unsigned long)),
         keybits.data());
   ioctl(fd, EVIOCGBIT(EV_ABS, absbits.size() * sizeof(unsigned long)),
         absbits.data());

   std::memset(pad.key_to_button, 0xff, sizeof(pad.key_to_button));
   std::memset(pad.abs_to_axis, 0xff, sizeof(pad.abs_to_axis));
   std::memset(pad.absinfo, 0, sizeof(pad.absinfo));

   unsigned button = 0;
   unsigned axis = 0;

   /* Must remain identical to RetroArch Stage3.15 fallback ordering. */
   for (int code = BTN_JOYSTICK;
        code <= KEY_MAX && button < H3531_JS_MAX_BUTTONS; ++code)
      if (h3531_front_test_bit((unsigned)code, keybits.data()))
         pad.key_to_button[code] = (uint8_t)button++;

   for (int code = 0;
        code < BTN_JOYSTICK && button < H3531_JS_MAX_BUTTONS; ++code)
      if (h3531_front_test_bit((unsigned)code, keybits.data()))
         pad.key_to_button[code] = (uint8_t)button++;

   for (int code = 0;
        code <= ABS_MAX && axis < H3531_JS_MAX_AXES; ++code)
   {
      input_absinfo info{};
      if (!h3531_front_test_bit((unsigned)code, absbits.data()))
         continue;
      if (ioctl(fd, EVIOCGABS(code), &info) < 0)
         continue;
      if (info.maximum <= info.minimum)
         continue;

      pad.abs_to_axis[code] = (uint8_t)axis;
      pad.absinfo[axis] = info;
      pad.axes[axis] = h3531_front_scale_abs(info, info.value);
      ++axis;
   }

   fprintf(stderr,
         "[GAMEFRONT] linuxraw event maps %s: %u buttons %u axes\n",
         pad.path.c_str(), button, axis);
}

static bool input_event_path_in_use(const Input &in, const char *path)
{
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (in.pads[i].fd >= 0 && in.pads[i].path == path) return true;
   return false;
}

static void input_scan_gamepads(Input &in)
{
   for (unsigned ev = 0; ev < H3531_EVENT_MAX_FRONT; ++ev)
   {
      int slot = -1;
      for (int p = 0; p < H3531_MAX_GAMEPADS; ++p)
         if (in.pads[p].fd < 0) { slot = p; break; }
      if (slot < 0) return;

      char path[64];
      snprintf(path, sizeof(path), "/dev/input/event%u", ev);
      if (input_event_path_in_use(in, path)) continue;
      if (in.fd >= 0 && in.path == path) continue;

      int fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0) continue;

      if (!input_event_gamepad_capable(fd))
      {
         close(fd);
         continue;
      }

      char name[128]{};
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
         snprintf(name, sizeof(name), "event%u", ev);

      GamepadInput &pad = in.pads[slot];
      pad = GamepadInput{};
      pad.fd = fd;
      pad.path = path;
      pad.name = name;

      input_build_event_maps(pad, fd);
      h3531_profile_load(pad);

      fprintf(stderr,
            "[GAMEFRONT] linuxraw event-fallback gamepad%d ready %s (%s) profile=%s\n",
            slot + 1, path, name,
            pad.profile.loaded ? "yes" : "bootstrap");
   }
}

static void input_watch_init(Input &in)
{
   in.watch_fd = inotify_init();
   in.watch_wd = -1;

   if (in.watch_fd < 0)
      return;

   fcntl(in.watch_fd, F_SETFL,
         fcntl(in.watch_fd, F_GETFL) | O_NONBLOCK);
   in.watch_wd = inotify_add_watch(in.watch_fd, "/dev/input",
         IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO | IN_MOVED_FROM);

   if (in.watch_wd < 0)
   {
      close(in.watch_fd);
      in.watch_fd = -1;
   }
}

static void input_watch_poll(Input &in)
{
   if (in.watch_fd < 0) return;

   unsigned char buf[2048];
   for (;;)
   {
      const ssize_t n = read(in.watch_fd, buf, sizeof(buf));
      if (n < 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
         return;
      }
      if (n == 0) return;

      ssize_t off = 0;
      while (off + (ssize_t)sizeof(inotify_event) <= n)
      {
         const inotify_event *ev =
            reinterpret_cast<const inotify_event*>(buf + off);
         if (ev->len && !std::strncmp(ev->name, "event", 5))
            in.rescan_requested = true;
         off += (ssize_t)sizeof(inotify_event) + ev->len;
      }
   }
}

static void input_rescan(Input &in)
{
   const uint64_t now = input_now_ms();

   if (in.watch_fd < 0 && now >= in.next_fallback_scan_ms)
   {
      in.rescan_requested = true;
      in.next_fallback_scan_ms = now + 5000ULL;
   }

   if (!in.rescan_requested) return;
   in.rescan_requested = false;

   if (in.fd < 0)
      input_keyboard_open(in);

   input_scan_gamepads(in);
}

static bool input_open(Input &in)
{
   in.rescan_requested = true;
   in.next_fallback_scan_ms = 0;
   input_watch_init(in);
   input_rescan(in);

   if (in.fd < 0)
      fprintf(stderr, "[GAMEFRONT] keyboard absent; event hotplug active\n");

   bool have_pad = false;
   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
      if (in.pads[i].fd >= 0) have_pad = true;

   if (!have_pad)
      fprintf(stderr,
            "[GAMEFRONT] gamepad event node absent; inotify hotplug active\n");

   return in.fd >= 0 || have_pad;
}

static void input_close(Input &in)
{
   if (in.fd >= 0) close(in.fd);
   in.fd = -1;
   in.path.clear();

   for (int i = 0; i < H3531_MAX_GAMEPADS; ++i)
   {
      if (in.pads[i].fd >= 0) close(in.pads[i].fd);
      in.pads[i] = GamepadInput{};
   }

   if (in.watch_wd >= 0 && in.watch_fd >= 0)
      inotify_rm_watch(in.watch_fd, in.watch_wd);
   if (in.watch_fd >= 0)
      close(in.watch_fd);

   in.watch_fd = -1;
   in.watch_wd = -1;
   in.rescan_requested = true;
   in.next_fallback_scan_ms = 0;
}

static void gamepad_disconnect(GamepadInput &pad)
{
   if (pad.fd >= 0) close(pad.fd);
   fprintf(stderr,
         "[GAMEFRONT] event gamepad disconnected %s; waiting for inotify hotplug\n",
         pad.path.empty() ? "<unknown>" : pad.path.c_str());
   pad = GamepadInput{};
}

static Action input_poll(Input &in)
{
   input_watch_poll(in);
   input_rescan(in);

   if (in.fd >= 0)
   {
      input_event ev{};
      for (;;)
      {
         ssize_t n = read(in.fd, &ev, sizeof(ev));
         if (n == (ssize_t)sizeof(ev))
         {
            if (ev.type != EV_KEY || (ev.value != 1 && ev.value != 2))
               continue;
            Action a = keyboard_action(ev.code);
            if (a != Action::None) return a;
            continue;
         }
         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR))
            break;
         if (n <= 0)
         {
            fprintf(stderr,
                  "[GAMEFRONT] keyboard disconnected %s; waiting for hotplug\n",
                  in.path.empty() ? "<unknown>" : in.path.c_str());
            close(in.fd);
            in.fd = -1;
            in.path.clear();
            in.rescan_requested = true;
         }
         break;
      }
   }

   for (int p = 0; p < H3531_MAX_GAMEPADS; ++p)
   {
      GamepadInput &pad = in.pads[p];
      if (pad.fd < 0) continue;

      for (;;)
      {
         input_event ev{};
         const ssize_t n = read(pad.fd, &ev, sizeof(ev));

         if (n == (ssize_t)sizeof(ev))
         {
            if (ev.type == EV_KEY && ev.code <= KEY_MAX)
            {
               const unsigned idx = pad.key_to_button[ev.code];
               if (idx < H3531_JS_MAX_BUTTONS)
               {
                  pad.buttons[idx] = ev.value != 0;
                  if (ev.value)
                  {
                     Action a = h3531_profile_button_action(pad, idx);
                     if (a != Action::None) return a;
                  }
               }
            }
            else if (ev.type == EV_ABS && ev.code <= ABS_MAX)
            {
               const unsigned idx = pad.abs_to_axis[ev.code];
               if (idx < H3531_JS_MAX_AXES)
               {
                  const int16_t value =
                     h3531_front_scale_abs(pad.absinfo[idx], ev.value);
                  pad.axes[idx] = value;
                  Action a = h3531_profile_axis_action(pad, idx, value);
                  if (a != Action::None) return a;
               }
            }
            continue;
         }

         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR))
            break;

         if (n <= 0)
         {
            gamepad_disconnect(pad);
            in.rescan_requested = true;
         }
         break;
      }
   }

   return Action::None;
}
'''

# Replace old JS transport block from path-use helper through input_close.
start = src.find("static bool input_js_path_in_use")
end = src.find("static Action keyboard_action", start)
if start < 0 or end < 0:
    raise SystemExit("Stage4.34 JS transport block not found")
src = src[:start] + transport + "\n" + src[end:]

wizard_drain = r'''static void stage434_drain_pad(GamepadInput &pad)
{
   if (pad.fd < 0) return;

   for (;;)
   {
      input_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));
      if (n != (ssize_t)sizeof(ev)) break;

      if (ev.type == EV_KEY && ev.code <= KEY_MAX)
      {
         const unsigned idx = pad.key_to_button[ev.code];
         if (idx < H3531_JS_MAX_BUTTONS)
            pad.buttons[idx] = ev.value != 0;
      }
      else if (ev.type == EV_ABS && ev.code <= ABS_MAX)
      {
         const unsigned idx = pad.abs_to_axis[ev.code];
         if (idx < H3531_JS_MAX_AXES)
            pad.axes[idx] =
               h3531_front_scale_abs(pad.absinfo[idx], ev.value);
      }
   }
}'''

wizard_wait = r'''static bool stage434_wait_buttons_released(GamepadInput &pad, Input &in)
{
   const uint64_t deadline = input_now_ms() + 2500ULL;

   while (input_now_ms() < deadline)
   {
      if (stage434_keyboard_cancel(in)) return false;

      bool released = true;
      for (int i = 0; i < H3531_JS_MAX_BUTTONS; ++i)
         if (pad.buttons[i]) released = false;

      if (released) return true;

      input_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.code <= KEY_MAX)
         {
            const unsigned idx = pad.key_to_button[ev.code];
            if (idx < H3531_JS_MAX_BUTTONS)
               pad.buttons[idx] = ev.value != 0;
         }
         else if (ev.type == EV_ABS && ev.code <= ABS_MAX)
         {
            const unsigned idx = pad.abs_to_axis[ev.code];
            if (idx < H3531_JS_MAX_AXES)
               pad.axes[idx] =
                  h3531_front_scale_abs(pad.absinfo[idx], ev.value);
         }
      }
      else
         usleep(5000);
   }

   return true;
}'''

wizard_capture = r'''static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,
      Input &in, GamepadInput &pad, int step, Stage434CapturedBind &out)
{
   stage434_draw(back.fb, pad, step, "");
   stage42_present(physical, back);

   stage434_drain_pad(pad);
   if (!stage434_wait_buttons_released(pad, in))
      return false;

   int16_t axis_baseline[H3531_JS_MAX_AXES]{};
   for (int i = 0; i < H3531_JS_MAX_AXES; ++i)
      axis_baseline[i] = pad.axes[i];

   for (;;)
   {
      if (stage434_keyboard_cancel(in))
         return false;

      input_event ev{};
      const ssize_t n = read(pad.fd, &ev, sizeof(ev));

      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.code <= KEY_MAX)
         {
            const unsigned idx = pad.key_to_button[ev.code];
            if (idx < H3531_JS_MAX_BUTTONS)
            {
               pad.buttons[idx] = ev.value != 0;
               if (ev.value)
               {
                  out.button = (int)idx;
                  out.axis = -1;
                  out.axis_dir = 0;
                  return true;
               }
            }
         }
         else if (ev.type == EV_ABS && ev.code <= ABS_MAX)
         {
            const unsigned idx = pad.abs_to_axis[ev.code];
            if (idx < H3531_JS_MAX_AXES)
            {
               const int16_t value =
                  h3531_front_scale_abs(pad.absinfo[idx], ev.value);
               pad.axes[idx] = value;
               const int delta = (int)value - (int)axis_baseline[idx];

               if (stage434_steps[step].direction &&
                   std::abs(delta) > 16000)
               {
                  out.button = -1;
                  out.axis = (int)idx;
                  out.axis_dir = value < 0 ? -1 : 1;
                  return true;
               }
            }
         }
         continue;
      }

      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR)
         return false;

      usleep(5000);
   }
}'''

src = replace_function(src, "static void stage434_drain_pad(GamepadInput &pad)", wizard_drain)
src = replace_function(src, "static bool stage434_wait_buttons_released(GamepadInput &pad, Input &in)", wizard_wait)
src = replace_function(src, "static bool stage434_capture(Fb &physical, Stage42Backbuffer &back,", wizard_capture)

src = src.replace(
    "NO /DEV/INPUT/JS CONTROLLER FOUND",
    "NO GAMEPAD EVENT DEVICE FOUND")
src = src.replace(
    "STANDARD RETROARCH LINUXRAW PROFILE",
    "RETROARCH LINUXRAW PROFILE / EVENT FALLBACK")
src = src.replace(
    "Stage4.34 RetroArch controller profile wizard active",
    "Stage4.35 RetroArch event-fallback controller wizard active")

required = [
    "Stage4.35 RetroArch event-fallback controller wizard active",
    "linuxraw event-fallback gamepad%d ready",
    "input_event_gamepad_capable",
    "input_build_event_maps",
    "RETROARCH LINUXRAW PROFILE / EVENT FALLBACK",
    "standard RetroArch controller profile saved",
]

for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.35 marker: " + marker)

for forbidden in [
    "/dev/input/js%d",
    "JS_EVENT_BUTTON",
    "JS_EVENT_AXIS",
    "NO /DEV/INPUT/JS CONTROLLER FOUND",
]:
    if forbidden in src:
        raise SystemExit("Stage4.35 still contains JS transport marker: " + forbidden)

out_path.write_text(src, encoding="utf-8")
print("STAGE435_EVENT_TRANSPORT_PATCH_OK")
