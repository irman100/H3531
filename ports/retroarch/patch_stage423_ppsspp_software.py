#!/usr/bin/env python3
"""Patch pinned PPSSPP libretro source for H3531 software-only rendering.

The Hi3531 target used by this project has no usable RetroArch GL/Vulkan path.
Keep the ARM dynarec but remove hardware-renderer dependencies and force the
existing PPSSPP LibretroSoftwareContext/SoftGPU path.
"""
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch_stage423_ppsspp_software.py PPSSPP_ROOT")
root = Path(sys.argv[1])

# 1) Linux normally enables GL in ppsspp_config.h. Give this target an explicit
# software-only platform gate instead of pretending to be another OS.
p = root / "ppsspp_config.h"
s = p.read_text(encoding="utf-8")
needle = "#if PPSSPP_PLATFORM(WINDOWS)\n#if PPSSPP_ARCH(ARM64) || PPSSPP_ARCH(ARM) || PPSSPP_PLATFORM(UWP)"
repl = "#if defined(H3531_SOFTWARE_ONLY)\n// H3531 libretro target: framebuffer software renderer, no GL API.\n#elif PPSSPP_PLATFORM(WINDOWS)\n#if PPSSPP_ARCH(ARM64) || PPSSPP_ARCH(ARM) || PPSSPP_PLATFORM(UWP)"
if needle not in s:
    raise SystemExit("ppsspp_config GL gate anchor not found")
s = s.replace(needle, repl, 1)
p.write_text(s, encoding="utf-8")

# 2) GPU factory: do not reference Vulkan/GLES classes in the software-only build.
p = root / "GPU" / "GPU.cpp"
s = p.read_text(encoding="utf-8")
s = s.replace('#include "GPU/Vulkan/GPU_Vulkan.h"', '#ifndef H3531_SOFTWARE_ONLY\n#include "GPU/Vulkan/GPU_Vulkan.h"\n#endif', 1)
needle = '''#if !PPSSPP_PLATFORM(SWITCH) && !PPSSPP_PLATFORM(UWP)\n\tcase GPUCORE_VULKAN:\n\t\tif (!ctx) {\n\t\t\t// Can this happen?\n\t\t\tERROR_LOG(Log::G3D, "Unable to init Vulkan GPU backend, no context");\n\t\t\treturn nullptr;\n\t\t}\n\t\treturn new GPU_Vulkan(ctx, draw);\n#endif'''
repl = '''#if !defined(H3531_SOFTWARE_ONLY) && !PPSSPP_PLATFORM(SWITCH) && !PPSSPP_PLATFORM(UWP)\n\tcase GPUCORE_VULKAN:\n\t\tif (!ctx) {\n\t\t\tERROR_LOG(Log::G3D, "Unable to init Vulkan GPU backend, no context");\n\t\t\treturn nullptr;\n\t\t}\n\t\treturn new GPU_Vulkan(ctx, draw);\n#endif'''
if needle not in s:
    raise SystemExit("GPU Vulkan factory anchor not found")
s = s.replace(needle, repl, 1)
p.write_text(s, encoding="utf-8")

# 3) Libretro context factory: bypass every HW backend and directly use PPSSPP's
# built-in software context. The software context already emits a 480x272 frame.
p = root / "libretro" / "LibretroGraphicsContext.cpp"
s = p.read_text(encoding="utf-8")
s = s.replace('#include "libretro/LibretroGLContext.h"\n#include "libretro/LibretroGLCoreContext.h"\n#include "libretro/LibretroVulkanContext.h"',
'''#ifndef H3531_SOFTWARE_ONLY\n#include "libretro/LibretroGLContext.h"\n#include "libretro/LibretroGLCoreContext.h"\n#include "libretro/LibretroVulkanContext.h"\n#endif''', 1)
anchor = '''LibretroGraphicsContext *LibretroGraphicsContext::CreateGraphicsContext() {\n\tLibretroGraphicsContext *ctx;\n'''
if anchor not in s:
    raise SystemExit("CreateGraphicsContext anchor not found")
s = s.replace(anchor, anchor + '''#ifdef H3531_SOFTWARE_ONLY\n\tstd::string errorMessage;\n\tctx = new LibretroSoftwareContext();\n\tctx->InitAPI(nullptr, nullptr, &errorMessage);\n\tINFO_LOG(Log::System, "H3531 software-only libretro graphics context active");\n\treturn ctx;\n#else\n''', 1)
end_anchor = '''\tctx = new LibretroSoftwareContext();\n\tctx->InitAPI(nullptr, nullptr, &errorMessage);\n\treturn ctx;\n}\n'''
if end_anchor not in s:
    raise SystemExit("CreateGraphicsContext end anchor not found")
s = s.replace(end_anchor, '''\tctx = new LibretroSoftwareContext();\n\tctx->InitAPI(nullptr, nullptr, &errorMessage);\n\treturn ctx;\n#endif\n}\n''', 1)
p.write_text(s, encoding="utf-8")

# 4) The upstream libretro makefile lists GL/Vulkan/VMA/VR sources even when
# the software renderer is selected. Filter every hardware-only family before
# OBJECTS is constructed. Keep generic thin3d/common GPU pieces required by
# the software backend, but remove API-specific implementations.
p = root / "libretro" / "Makefile"
s = p.read_text(encoding="utf-8")
needle = '''include Makefile.common\n\nifeq ($(GLES), 1)\n\tGLFLAGS += -DGLES -DUSING_GLES2\nelse\n\tGLFLAGS += -DHAVE_OPENGL\nendif\n'''
repl = '''include Makefile.common\n\nifeq ($(H3531_SOFTWARE_ONLY),1)\nSOURCES_CXX := $(filter-out \\\n  $(COMMONDIR)/GPU/OpenGL/% \\\n  $(COMMONDIR)/GPU/Vulkan/% \\\n  $(COMMONDIR)/VR/% \\\n  $(GPUDIR)/GLES/% \\\n  $(GPUDIR)/Vulkan/% \\\n  $(EXTDIR)/vma/% \\\n  $(EXTDIR)/glslang/% \\\n  $(EXTDIR)/SPIRV-Cross/% \\\n  $(LIBRETRODIR)/LibretroGLContext.cpp \\\n  $(LIBRETRODIR)/LibretroGLCoreContext.cpp \\\n  $(LIBRETRODIR)/LibretroVulkanContext.cpp \\\n  $(LIBRETRODIR)/LibretroVulkanPresentation.cpp,$(SOURCES_CXX))\nSOURCES_C := $(filter-out \\\n  $(COMMONDIR)/GPU/OpenGL/% \\\n  $(LIBRETRODIR)/ext/glew/%,$(SOURCES_C))\nGL_LIB :=\nGLFLAGS := -DH3531_SOFTWARE_ONLY\nCOREFLAGS := $(filter-out -DVK_USE_PLATFORM_XLIB_KHR -DGLEW_STATIC -DGLEW_NO_GLU,$(COREFLAGS))\nelse\nifeq ($(GLES), 1)\n\tGLFLAGS += -DGLES -DUSING_GLES2\nelse\n\tGLFLAGS += -DHAVE_OPENGL\nendif\nendif\n'''
if needle not in s:
    raise SystemExit("Makefile GLFLAGS anchor not found")
s = s.replace(needle, repl, 1)
p.write_text(s, encoding="utf-8")

print("STAGE423_PPSSPP_SOFTWARE_ONLY patched")
