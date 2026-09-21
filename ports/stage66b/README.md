# H3531 Stage6.6B - Application Framework

Stage6.6B makes applications discoverable by the desktop through APPINFO.CFG manifests.

FBZX is the first proof:
- window mode uses X11 and -ds to disable simulated CRT scanlines;
- fullscreen mode uses X11 with -ds -fs;
- F9 remains the runtime window/fullscreen toggle;
- Spectrum files default to the windowed launcher.

128K profile:
- uses a separate HOME at /var/h3531-fbzx-128;
- seeds mode=1 and AY sound enabled;
- keeps 48K settings isolated;
- 128K TAP/TZX launch must use the 128K profile because tape images do not identify machine type.
