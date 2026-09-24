/* RetroArch H3531 evdev keyboard input.
 *
 * Stage3.14 architecture:
 *   - keyboard: H3531 evdev (/dev/input/event*) with hotplug;
 *   - joypads: stock RetroArch linuxraw joypad driver (/dev/input/js*);
 *   - standard RetroArch autoconfig/auto_binds are authoritative.
 *
 * This deliberately removes the old per-frame H3531 gamepad event scan and
 * hard-coded BTN_/ABS_ mapping. The stock linuxraw joypad driver uses
 * epoll+inotify and therefore does not perform periodic event0..63 scans.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../verbosity.h"
#include "../input_keymaps.h"
#include "../input_driver.h"

#define H3531_EVENT_MAX 64
#define H3531_BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define H3531_NBITS(x) (((x) + H3531_BITS_PER_LONG - 1U) / H3531_BITS_PER_LONG)
#define H3531_TEST_BIT(bit, arr) \
   (((arr)[(unsigned)(bit) / H3531_BITS_PER_LONG] >> \
      ((unsigned)(bit) % H3531_BITS_PER_LONG)) & 1UL)

typedef struct linuxraw_input
{
   bool state[KEY_MAX + 1];
   int fd;
   char path[64];

   int watch_fd;
   int watch_wd;
   uint64_t fallback_scan_ms;

   const input_device_driver_t *joypad;
} linuxraw_input_t;

static uint64_t h3531_now_ms(void)
{
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static int h3531_contains_ci(const char *s, const char *needle)
{
   size_t i, j;
   if (!s || !needle || !needle[0])
      return 0;

   for (i = 0; s[i]; ++i)
   {
      for (j = 0; needle[j] && s[i + j]; ++j)
      {
         char a = s[i + j];
         char b = needle[j];
         if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
         if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
         if (a != b)
            break;
      }
      if (!needle[j])
         return 1;
   }
   return 0;
}

static bool h3531_keyboard_capable(int fd, const char *name)
{
   unsigned long evbits[H3531_NBITS(EV_MAX + 1)];
   unsigned long keybits[H3531_NBITS(KEY_MAX + 1)];
   unsigned score = 0;

   memset(evbits, 0, sizeof(evbits));
   memset(keybits, 0, sizeof(keybits));

   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
      return false;
   if (!H3531_TEST_BIT(EV_KEY, evbits))
      return false;
   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
      return false;

   if (H3531_TEST_BIT(KEY_A, keybits)) score++;
   if (H3531_TEST_BIT(KEY_Z, keybits)) score++;
   if (H3531_TEST_BIT(KEY_ENTER, keybits)) score++;
   if (H3531_TEST_BIT(KEY_ESC, keybits)) score++;
   if (H3531_TEST_BIT(KEY_UP, keybits)) score++;
   if (H3531_TEST_BIT(KEY_DOWN, keybits)) score++;
   if (H3531_TEST_BIT(KEY_LEFT, keybits)) score++;
   if (H3531_TEST_BIT(KEY_RIGHT, keybits)) score++;

   if (h3531_contains_ci(name, "keyboard") || h3531_contains_ci(name, "kbd"))
      return score >= 3;

   return score >= 7;
}

static int h3531_open_keyboard(char *chosen, size_t chosen_len)
{
   const char *forced = getenv("RETROARCH_INPUT");
   int i;

   if (forced && forced[0])
   {
      int fd = open(forced, O_RDONLY | O_NONBLOCK);
      if (fd >= 0)
      {
         char name[128] = {0};
         if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
            snprintf(name, sizeof(name), "forced");
         if (h3531_keyboard_capable(fd, name))
         {
            snprintf(chosen, chosen_len, "%s", forced);
            if (ioctl(fd, EVIOCGRAB, (void*)1) < 0)
               RARCH_WARN("[H3531] keyboard EVIOCGRAB failed on %s: %s\n",
                     forced, strerror(errno));
            RARCH_LOG("[H3531] evdev keyboard forced: %s (%s)\n", forced, name);
            return fd;
         }
         close(fd);
      }
   }

   for (i = 0; i < H3531_EVENT_MAX; ++i)
   {
      char path[64];
      char name[128];
      int fd;

      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0)
         continue;

      memset(name, 0, sizeof(name));
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
         snprintf(name, sizeof(name), "event%d", i);

      if (h3531_keyboard_capable(fd, name))
      {
         snprintf(chosen, chosen_len, "%s", path);
         if (ioctl(fd, EVIOCGRAB, (void*)1) < 0)
            RARCH_WARN("[H3531] keyboard EVIOCGRAB failed on %s: %s\n",
                  path, strerror(errno));
         RARCH_LOG("[H3531] evdev keyboard: %s (%s)\n", path, name);
         return fd;
      }

      close(fd);
   }

   return -1;
}

static void h3531_keyboard_attach(linuxraw_input_t *in)
{
   if (!in || in->fd >= 0)
      return;

   in->fd = h3531_open_keyboard(in->path, sizeof(in->path));
   if (in->fd >= 0)
      RARCH_LOG("[H3531] keyboard attached without restarting RetroArch\n");
}

static void h3531_keyboard_watch_init(linuxraw_input_t *in)
{
   if (!in)
      return;

   in->watch_fd = inotify_init();
   in->watch_wd = -1;

   if (in->watch_fd < 0)
   {
      RARCH_WARN("[H3531] inotify unavailable; keyboard fallback scan enabled\n");
      return;
   }

   fcntl(in->watch_fd, F_SETFL,
         fcntl(in->watch_fd, F_GETFL) | O_NONBLOCK);
   in->watch_wd = inotify_add_watch(in->watch_fd, "/dev/input",
         IN_CREATE | IN_ATTRIB | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);

   if (in->watch_wd < 0)
   {
      RARCH_WARN("[H3531] cannot watch /dev/input: %s\n", strerror(errno));
      close(in->watch_fd);
      in->watch_fd = -1;
   }
}

static bool h3531_keyboard_hotplug_event(linuxraw_input_t *in)
{
   unsigned char buf[1024];
   bool changed = false;

   if (!in || in->watch_fd < 0)
      return false;

   for (;;)
   {
      ssize_t n = read(in->watch_fd, buf, sizeof(buf));
      ssize_t off = 0;

      if (n < 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            break;
         RARCH_WARN("[H3531] inotify read failed: %s\n", strerror(errno));
         break;
      }
      if (n == 0)
         break;

      while (off + (ssize_t)sizeof(struct inotify_event) <= n)
      {
         const struct inotify_event *ev =
            (const struct inotify_event*)(buf + off);

         if (ev->len && !strncmp(ev->name, "event", 5))
            changed = true;

         off += (ssize_t)sizeof(struct inotify_event) + ev->len;
      }
   }

   return changed;
}

static bool h3531_key_down(const linuxraw_input_t *in, enum retro_key key)
{
   unsigned sym;
   if (!in || key <= RETROK_UNKNOWN || key >= RETROK_LAST)
      return false;

   sym = rarch_keysym_lut[key];
   if (sym > KEY_MAX)
      return false;
   return in->state[sym];
}

static bool h3531_joypad_bind_pressed(
      const linuxraw_input_t *in,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      unsigned port,
      unsigned id)
{
   const uint64_t joykey = (binds[port][id].joykey != NO_BTN)
      ? binds[port][id].joykey : joypad_info->auto_binds[id].joykey;
   const uint32_t joyaxis = (binds[port][id].joyaxis != AXIS_NONE)
      ? binds[port][id].joyaxis : joypad_info->auto_binds[id].joyaxis;

   if (!in || !in->joypad || !joypad_info)
      return false;

   if ((uint16_t)joykey != NO_BTN &&
       in->joypad->button(joypad_info->joy_idx, (uint16_t)joykey))
      return true;

   if (joyaxis != AXIS_NONE &&
       ((float)abs(in->joypad->axis(joypad_info->joy_idx, joyaxis))
         / 0x8000) > joypad_info->axis_threshold)
      return true;

   return false;
}

static int16_t h3531_joypad_analog(
      const linuxraw_input_t *in,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      unsigned port,
      unsigned idx,
      unsigned id)
{
   unsigned id_minus = 0;
   unsigned id_plus  = 0;
   uint32_t axis_minus;
   uint32_t axis_plus;
   int16_t value;

   if (!in || !in->joypad || !joypad_info || !binds)
      return 0;

   input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

   axis_minus = (binds[port][id_minus].joyaxis != AXIS_NONE)
      ? binds[port][id_minus].joyaxis
      : joypad_info->auto_binds[id_minus].joyaxis;
   axis_plus = (binds[port][id_plus].joyaxis != AXIS_NONE)
      ? binds[port][id_plus].joyaxis
      : joypad_info->auto_binds[id_plus].joyaxis;

   if (axis_plus != AXIS_NONE)
   {
      value = in->joypad->axis(joypad_info->joy_idx, axis_plus);
      if (value > 0)
         return value;
   }

   if (axis_minus != AXIS_NONE)
   {
      value = in->joypad->axis(joypad_info->joy_idx, axis_minus);
      if (value < 0)
         return value;
   }

   return 0;
}

static void *linuxraw_input_init(const char *joypad_driver)
{
   linuxraw_input_t *in = (linuxraw_input_t*)calloc(1, sizeof(*in));

   if (!in)
      return NULL;

   in->fd = -1;
   in->watch_fd = -1;
   in->watch_wd = -1;
   in->fallback_scan_ms = 0;

   input_keymaps_init_keyboard_lut(rarch_key_map_linux);

   h3531_keyboard_watch_init(in);
   h3531_keyboard_attach(in);

   in->joypad = input_joypad_init_driver(joypad_driver, in);
   if (in->joypad)
      RARCH_LOG("[H3531] standard RetroArch joypad driver: %s\n",
            in->joypad->ident ? in->joypad->ident : "<unknown>");
   else
      RARCH_WARN("[H3531] no RetroArch joypad driver initialized\n");

   RARCH_LOG("[H3531] keyboard + standard linuxraw joypad input initialized\n");
   return in;
}

static int16_t linuxraw_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      bool keyboard_mapping_blocked,
      unsigned port,
      unsigned device,
      unsigned idx,
      unsigned id)
{
   linuxraw_input_t *in = (linuxraw_input_t*)data;
   (void)joypad;
   (void)sec_joypad;

   if (!in)
      return 0;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;

            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND && i < 16; ++i)
            {
               if (binds && joypad_info &&
                   h3531_joypad_bind_pressed(in, joypad_info, binds, port, i))
                  ret |= (int16_t)(1U << i);

               if (port == 0 && binds && !keyboard_mapping_blocked &&
                   RETRO_KEYBIND_VALID(&binds[port][i]))
               {
                  enum retro_key key =
                     (enum retro_key)RETRO_KEYBIND_KEY(&binds[port][i]);
                  if (h3531_key_down(in, key))
                     ret |= (int16_t)(1U << i);
               }
            }
            return ret;
         }

         if (id < RARCH_BIND_LIST_END && binds)
         {
            if (joypad_info &&
                h3531_joypad_bind_pressed(in, joypad_info, binds, port, id))
               return 1;

            if (port == 0 && !keyboard_mapping_blocked &&
                RETRO_KEYBIND_VALID(&binds[port][id]))
            {
               enum retro_key key =
                  (enum retro_key)RETRO_KEYBIND_KEY(&binds[port][id]);
               if (h3531_key_down(in, key))
                  return 1;
            }
         }
         break;

      case RETRO_DEVICE_ANALOG:
      {
         int16_t physical = h3531_joypad_analog(
               in, joypad_info, binds, port, idx, id);
         if (physical)
            return physical;

         if (port == 0 && binds)
         {
            unsigned id_minus = 0;
            unsigned id_plus  = 0;
            int16_t ret = 0;

            input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

            if (RETRO_KEYBIND_VALID(&binds[port][id_plus]) &&
                h3531_key_down(in, (enum retro_key)
                  RETRO_KEYBIND_KEY(&binds[port][id_plus])))
               ret = 0x7fff;

            if (RETRO_KEYBIND_VALID(&binds[port][id_minus]) &&
                h3531_key_down(in, (enum retro_key)
                  RETRO_KEYBIND_KEY(&binds[port][id_minus])))
               ret = (int16_t)(ret - 0x7fff);

            return ret;
         }
         break;
      }

      case RETRO_DEVICE_KEYBOARD:
         if (id && id < RETROK_LAST)
            return h3531_key_down(in, (enum retro_key)id) ? 1 : 0;
         break;
   }

   return 0;
}

static void linuxraw_input_poll(void *data)
{
   linuxraw_input_t *in = (linuxraw_input_t*)data;
   struct input_event ev;

   if (!in)
      return;

   if (in->joypad && in->joypad->poll)
      in->joypad->poll();

   if (h3531_keyboard_hotplug_event(in) && in->fd < 0)
      h3531_keyboard_attach(in);

   if (in->watch_fd < 0 && in->fd < 0)
   {
      uint64_t now = h3531_now_ms();
      if (now >= in->fallback_scan_ms)
      {
         in->fallback_scan_ms = now + 5000ULL;
         h3531_keyboard_attach(in);
      }
   }

   if (in->fd < 0)
      return;

   for (;;)
   {
      ssize_t n = read(in->fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.code <= KEY_MAX &&
             (ev.value == 0 || ev.value == 1 || ev.value == 2))
         {
            bool pressed = ev.value != 0;
            enum retro_key key;
            uint16_t mod = 0;

            in->state[ev.code] = pressed;

            if (in->state[KEY_LEFTCTRL] || in->state[KEY_RIGHTCTRL])
               mod |= RETROKMOD_CTRL;
            if (in->state[KEY_LEFTALT] || in->state[KEY_RIGHTALT])
               mod |= RETROKMOD_ALT;
            if (in->state[KEY_LEFTSHIFT] || in->state[KEY_RIGHTSHIFT])
               mod |= RETROKMOD_SHIFT;
            if (in->state[KEY_LEFTMETA] || in->state[KEY_RIGHTMETA])
               mod |= RETROKMOD_META;

            key = input_keymaps_translate_keysym_to_rk(ev.code);
            if (key > RETROK_UNKNOWN && key < RETROK_LAST)
               input_keyboard_event(pressed, key, key, mod,
                     RETRO_DEVICE_KEYBOARD);
         }
         continue;
      }

      if (n < 0)
      {
         int read_errno = errno;

         if (read_errno == EAGAIN || read_errno == EWOULDBLOCK ||
             read_errno == EINTR)
            break;

         if (read_errno != ENODEV && read_errno != ENXIO &&
             read_errno != EIO && read_errno != EBADF)
         {
            RARCH_WARN("[H3531] transient keyboard read error on %s: %s\n",
                  in->path[0] ? in->path : "event device",
                  strerror(read_errno));
            break;
         }
      }
      else if (n != 0)
      {
         RARCH_WARN("[H3531] short keyboard read on %s: %ld bytes\n",
               in->path[0] ? in->path : "event device", (long)n);
         break;
      }

      RARCH_WARN("[H3531] keyboard disconnected: %s\n",
            in->path[0] ? in->path : "event device");
      ioctl(in->fd, EVIOCGRAB, (void*)0);
      close(in->fd);
      in->fd = -1;
      in->path[0] = '\0';
      memset(in->state, 0, sizeof(in->state));
      break;
   }
}

static void linuxraw_input_free(void *data)
{
   linuxraw_input_t *in = (linuxraw_input_t*)data;

   if (!in)
      return;

   if (in->fd >= 0)
   {
      ioctl(in->fd, EVIOCGRAB, (void*)0);
      close(in->fd);
   }

   if (in->joypad && in->joypad->destroy)
      in->joypad->destroy();

   if (in->watch_wd >= 0 && in->watch_fd >= 0)
      inotify_rm_watch(in->watch_fd, in->watch_wd);
   if (in->watch_fd >= 0)
      close(in->watch_fd);

   free(in);
}

static bool linuxraw_input_set_sensor_state(void *data, unsigned port,
      enum retro_sensor_action action, unsigned rate)
{
   (void)data; (void)port; (void)action; (void)rate;
   return false;
}

static float linuxraw_input_get_sensor_input(void *data, unsigned port,
      unsigned id)
{
   (void)data; (void)port; (void)id;
   return 0.0f;
}

static uint64_t linuxraw_get_capabilities(void *data)
{
   (void)data;
   return (1ULL << RETRO_DEVICE_JOYPAD)
        | (1ULL << RETRO_DEVICE_ANALOG)
        | (1ULL << RETRO_DEVICE_KEYBOARD);
}

input_driver_t input_linuxraw = {
   linuxraw_input_init,
   linuxraw_input_poll,
   linuxraw_input_state,
   linuxraw_input_free,
   linuxraw_input_set_sensor_state,
   linuxraw_input_get_sensor_input,
   linuxraw_get_capabilities,
   "h3531evdev",
   NULL,
   NULL,
   NULL
};
