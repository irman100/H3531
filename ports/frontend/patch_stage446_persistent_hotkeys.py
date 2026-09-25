#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: patch_stage446_persistent_hotkeys.py INPUT OUTPUT')

src_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
src = src_path.read_text(encoding='utf-8')

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

helpers = r'''
static const char *STAGE446_HOTKEYS =
   "/mnt/usb/H3531/USER/gamefront/hotkeys.cfg";
static const char *STAGE446_APP_RA_CFG =
   "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg";

static const char *stage446_hotkey_bases[] = {
   "enable_hotkey",
   "rewind",
   "menu_toggle",
   "save_state",
   "load_state",
   "hold_fast_forward",
   "exit_emulator",
};

static bool stage446_hotkey_key_allowed(const std::string &key)
{
   const int count = (int)(sizeof(stage446_hotkey_bases) /
         sizeof(stage446_hotkey_bases[0]));
   for (int i = 0; i < count; ++i)
   {
      const std::string stem =
         std::string("input_") + stage446_hotkey_bases[i];
      if (key == stem + "_btn" || key == stem + "_axis")
         return true;
   }
   return false;
}

static std::string stage446_unquote(std::string v)
{
   v = trim(v);
   if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
      v = v.substr(1, v.size() - 2);
   return v;
}

static std::map<std::string, std::string> stage446_read_hotkey_values(
      const char *path)
{
   std::map<std::string, std::string> values;
   std::ifstream in(path);
   std::string line;

   while (std::getline(in, line))
   {
      const size_t hash = line.find('#');
      if (hash != std::string::npos)
         line.resize(hash);

      const size_t p = line.find('=');
      if (p == std::string::npos)
         continue;

      const std::string key = trim(line.substr(0, p));
      if (!stage446_hotkey_key_allowed(key))
         continue;

      values[key] = stage446_unquote(line.substr(p + 1));
   }
   return values;
}

static void stage446_write_hotkey_values(
      const std::map<std::string, std::string> &values)
{
   stage413_dirs();
   const std::string tmp = std::string(STAGE446_HOTKEYS) + ".tmp";
   std::ofstream out(tmp, std::ios::trunc);
   if (!out) return;

   for (const auto &kv : values)
      if (stage446_hotkey_key_allowed(kv.first))
         out << kv.first << " = \"" << kv.second << "\"\n";
   out.close();
   if (!out) return;

   if (rename(tmp.c_str(), STAGE446_HOTKEYS) != 0)
   {
      unlink(tmp.c_str());
      return;
   }
}

static void stage446_migrate_hotkeys()
{
   std::ifstream existing(STAGE446_HOTKEYS);
   if (existing.good())
      return;

   const auto values = stage446_read_hotkey_values(STAGE446_APP_RA_CFG);
   stage446_write_hotkey_values(values);
   fprintf(stderr,
         "[STAYPLAYTION] persistent hotkeys migrated: %s entries=%u\n",
         STAGE446_HOTKEYS, (unsigned)values.size());
}

static void stage446_append_hotkeys(std::ofstream &out)
{
   const auto values = stage446_read_hotkey_values(STAGE446_HOTKEYS);
   for (const auto &kv : values)
      if (stage446_hotkey_key_allowed(kv.first))
         out << kv.first << " = \"" << kv.second << "\"\n";

   fprintf(stderr,
         "[STAYPLAYTION] persistent hotkeys applied: %s entries=%u\n",
         STAGE446_HOTKEYS, (unsigned)values.size());
}
'''

sig = "static void stage413_write_ra_cfg(const SystemDef *runtime_sys = nullptr)"
pos = src.find(sig)
if pos < 0:
    raise SystemExit("Stage4.46 stage413_write_ra_cfg anchor missing")
src = src[:pos] + helpers + "\n" + src[pos:]

write_cfg = r'''static void stage413_write_ra_cfg(const SystemDef *runtime_sys = nullptr)
{
   stage413_dirs();
   std::ofstream out(STAGE413_RA_CFG, std::ios::trunc);
   if (!out) return;

   /* Runtime policy is regenerated on every launch. Persistent controller
    * hotkeys are stored separately under USER and appended below. */
   out << "savestate_auto_save = \"" << (stage413_settings.auto_save ? "true" : "false") << "\"\n";
   out << "savestate_auto_load = \"" << (stage413_settings.auto_load ? "true" : "false") << "\"\n";
   out << "savestate_thumbnail_enable = \"true\"\n";
   out << "state_slot = \"" << stage413_settings.state_slot << "\"\n";
   out << "rewind_enable = \"" << (stage413_settings.rewind ? "true" : "false") << "\"\n";
   out << "rewind_buffer_size = \"" << stage413_settings.rewind_buffer_mb << "\"\n";

   const bool stage445_ps1 =
      runtime_sys && lower(runtime_sys->name) == "ps1";
   const int stage445_granularity =
      stage445_ps1 ? 60 : stage413_settings.rewind_granularity;
   out << "rewind_granularity = \"" << stage445_granularity << "\"\n";

   stage446_append_hotkeys(out);

   if (runtime_sys)
      fprintf(stderr,
            "[GAMEFRONT] RUNTIME-POLICY system=%s rewind=%s granularity=%d\n",
            runtime_sys->name.c_str(),
            stage413_settings.rewind ? "on" : "off",
            stage445_granularity);
}'''
src = replace_function(src, sig, write_cfg)

cfg_bind = r'''static Stage434CapturedBind stage437_cfg_bind(const char *base)
{
   Stage434CapturedBind out;
   std::map<std::string, std::string> kv;

   /* Persistent USER hotkeys are authoritative. Fall back to the legacy
    * application config only for one-generation migration compatibility. */
   if (!h3531_read_cfg(STAGE446_HOTKEYS, kv))
      h3531_read_cfg(STAGE446_APP_RA_CFG, kv);

   const std::string stem = std::string("input_") + base;
   auto ib = kv.find(stem + "_btn");
   if (ib != kv.end() && !ib->second.empty() && ib->second != "nul")
   {
      char *end = nullptr;
      long n = std::strtol(ib->second.c_str(), &end, 10);
      if (end && *end == '\0' && n >= 0 && n < H3531_JS_MAX_BUTTONS)
         out.button = (int)n;
   }

   auto ia = kv.find(stem + "_axis");
   if (ia != kv.end() && ia->second.size() >= 2 &&
       (ia->second[0] == '+' || ia->second[0] == '-'))
   {
      char *end = nullptr;
      long n = std::strtol(ia->second.c_str() + 1, &end, 10);
      if (end && *end == '\0' && n >= 0 && n < H3531_JS_MAX_AXES)
      {
         out.axis = (int)n;
         out.axis_dir = ia->second[0] == '-' ? -1 : 1;
      }
   }

   return out;
}'''
src = replace_function(src,
      "static Stage434CapturedBind stage437_cfg_bind(const char *base)",
      cfg_bind)

write_binding = r'''static bool stage437_write_global_binding(
      const char *base,
      const Stage434CapturedBind &bind)
{
   stage413_dirs();

   std::map<std::string, std::string> values =
      stage446_read_hotkey_values(STAGE446_HOTKEYS);
   const std::string stem = std::string("input_") + base;

   if (bind.button >= 0)
   {
      values[stem + "_btn"] = std::to_string(bind.button);
      values[stem + "_axis"] = "nul";
   }
   else if (bind.axis >= 0 && bind.axis_dir)
   {
      values[stem + "_btn"] = "nul";
      values[stem + "_axis"] =
         std::string(bind.axis_dir < 0 ? "-" : "+") +
         std::to_string(bind.axis);
   }
   else
   {
      values[stem + "_btn"] = "nul";
      values[stem + "_axis"] = "nul";
   }

   stage446_write_hotkey_values(values);

   /* Keep the launch appendconfig immediately in sync as well. */
   stage413_write_ra_cfg();

   fprintf(stderr,
         "[STAYPLAYTION] persistent RetroArch hotkey saved: %s path=%s\n",
         base, STAGE446_HOTKEYS);
   return true;
}'''
src = replace_function(src,
      "static bool stage437_write_global_binding(",
      write_binding)

startup_old = '''   stage413_dirs();
   stage413_load_settings();
   stage413_load_favorites();
   stage413_write_ra_cfg();'''
startup_new = '''   stage413_dirs();
   stage446_migrate_hotkeys();
   stage413_load_settings();
   stage413_load_favorites();
   stage413_write_ra_cfg();'''
if startup_old not in src:
    raise SystemExit("Stage4.46 startup migration anchor missing")
src = src.replace(startup_old, startup_new, 1)

layout = '   printf("LAYOUT_TEST_OK\\n");\n'
marker = '   printf("STAGE446_HOTKEY_PERSISTENCE user-store all-systems ps1-exit-compatible\\n");\n'
if layout not in src:
    raise SystemExit("Stage4.46 layout anchor missing")
src = src.replace(layout, marker + layout, 1)

required = [
    "STAGE446_HOTKEY_PERSISTENCE user-store all-systems ps1-exit-compatible",
    "/mnt/usb/H3531/USER/gamefront/hotkeys.cfg",
    "persistent hotkeys migrated:",
    "persistent hotkeys applied:",
    "persistent RetroArch hotkey saved:",
    "stage446_append_hotkeys(out);",
    "stage446_migrate_hotkeys();",
]
for marker in required:
    if marker not in src:
        raise SystemExit("missing Stage4.46 marker: " + marker)

# The new writer must no longer modify the application-owned RetroArch config.
body_start = src.find("static bool stage437_write_global_binding(")
body_end = src.find("static const char *stage437_hotkey_actions[]", body_start)
if body_start < 0 or body_end < 0:
    raise SystemExit("Stage4.46 hotkey writer validation anchors missing")
writer_body = src[body_start:body_end]
if 'std::ofstream in("/mnt/usb/H3531/APPS/retroarch/retroarch.cfg")' in writer_body:
    raise SystemExit("legacy app-owned hotkey writer survived")

out_path.write_text(src, encoding='utf-8')
print('STAGE446_PERSISTENT_HOTKEYS_PATCH_OK')
