#!/usr/bin/env python3
from pathlib import Path
import sys
if len(sys.argv)!=3: raise SystemExit('usage: patch_stage444_quiescent_launch.py INPUT OUTPUT')
src=Path(sys.argv[1]).read_text()
old='''struct Stage42Backbuffer {\n   std::vector<uint16_t> pixels;\n   Fb fb;\n\n   bool init(const Fb &physical)\n   {\n      if (!physical.w || !physical.h) return false;\n      pixels.assign((size_t)physical.w * physical.h, pack1555(0, 0, 0));\n      fb = physical;\n      fb.fd = -1;\n      fb.mem = reinterpret_cast<uint8_t*>(pixels.data());\n      fb.stride = physical.w * 2U;\n      fb.len = pixels.size() * sizeof(uint16_t);\n      return true;\n   }\n};'''
new='''struct Stage42Backbuffer {\n   std::vector<uint16_t> pixels;\n   Fb fb;\n\n   bool init(const Fb &physical)\n   {\n      if (!physical.w || !physical.h) return false;\n      pixels.assign((size_t)physical.w * physical.h, pack1555(0, 0, 0));\n      fb = physical;\n      fb.fd = -1;\n      fb.mem = reinterpret_cast<uint8_t*>(pixels.data());\n      fb.stride = physical.w * 2U;\n      fb.len = pixels.size() * sizeof(uint16_t);\n      return true;\n   }\n\n   void release()\n   {\n      std::vector<uint16_t>().swap(pixels);\n      fb.mem = nullptr;\n      fb.fd = -1;\n      fb.w = fb.h = 0;\n      fb.stride = 0;\n      fb.len = 0;\n   }\n};'''
if old not in src: raise SystemExit('backbuffer anchor missing')
src=src.replace(old,new,1)
anchor='''static void stage421_draw_aura(Fb &fb, const std::string &path,\n      int x, int y, int w, int h, int outer, int strength)'''
pos=src.find(anchor)
if pos<0: raise SystemExit('aura anchor missing')
helper=r'''
static long stage444_proc_kb(const char *path, const char *key)
{
   std::ifstream f(path);
   if (!f) return -1;
   std::string name;
   long value = 0;
   while (f >> name >> value)
   {
      if (name == key) return value;
      std::string rest;
      std::getline(f, rest);
   }
   return -1;
}

static void stage444_log_memory(const char *tag)
{
   const long rss = stage444_proc_kb("/proc/self/status", "VmRSS:");
   const long size = stage444_proc_kb("/proc/self/status", "VmSize:");
   const long memfree = stage444_proc_kb("/proc/meminfo", "MemFree:");
   const long cached = stage444_proc_kb("/proc/meminfo", "Cached:");
   fprintf(stderr,
         "[GAMEFRONT] QUIESCE %s VmRSS=%ldkB VmSize=%ldkB MemFree=%ldkB Cached=%ldkB\\n",
         tag, rss, size, memfree, cached);
}

static void stage444_release_heavy_ui_caches(Stage42Backbuffer &back)
{
   stage444_log_memory("before-release");

   /* Every object below is a reconstructable presentation cache.  Keep the
    * library, Favorites, Search state, controller profile and settings intact. */
   std::vector<Stage42Art>().swap(stage42_art_cache);
   stage42_art_stamp = 1;
   stage45_asset_cache.clear();
   stage47_scaled_assets.clear();
   std::vector<Stage47CoverCache>().swap(stage47_cover_cache);
   stage47_cover_stamp = 1;
   stage48_exact_assets.clear();
   std::vector<Stage427CoverCache>().swap(stage427_cover_cache);
   stage427_cover_stamp = 1;
   stage421_bounds_cache.clear();
   stage421_aura_cache.clear();
   stage421_aura_stamp = 1;
   back.release();

   stage444_log_memory("after-release");
}

static int stage444_run_external_quiescent(Fb &physical, Input &in,
      Stage42Backbuffer &back, const std::string &cmd)
{
   stage444_release_heavy_ui_caches(back);
   fprintf(stderr, "[GAMEFRONT] QUIESCE external-launch caches=empty backbuffer=released\\n");
   const int rc = run_external(physical, in, cmd);
   stage444_log_memory("after-child");
   return rc;
}

'''
src=src[:pos]+helper+src[pos:]
# Replace only wrapper/main calls, not inherited backend function declaration.
needle='run_external(physical, in,'
helper_call='   const int rc = run_external(physical, in, cmd);'
if helper_call not in src: raise SystemExit('helper run_external anchor missing')
protected='   const int rc = __STAGE444_REAL_RUN_EXTERNAL__(physical, in, cmd);'
src=src.replace(helper_call, protected, 1)
count=src.count(needle)
if count != 5: raise SystemExit(f'expected 5 wrapper run_external calls, got {count}')
src=src.replace(needle, 'stage444_run_external_quiescent(physical, in, back,')
src=src.replace(protected, helper_call, 1)
layout='   printf("LAYOUT_TEST_OK\\n");\n'
marker='   printf("STAGE444_QUIESCENT external-launch releases-all-ui-caches-and-backbuffer keeps-features\\n");\n'
if layout not in src: raise SystemExit('layout anchor missing')
src=src.replace(layout,marker+layout,1)
for m in ['stage444_release_heavy_ui_caches','stage47_scaled_assets.clear()','stage421_aura_cache.clear()','back.release()','QUIESCE external-launch','STAGE444_QUIESCENT']:
    if m not in src: raise SystemExit('missing '+m)
Path(sys.argv[2]).write_text(src)
print('STAGE444_QUIESCENT_PATCH_OK')
