# H3531 Stage6.6C - Native App Bridge

Goal: run existing direct-HIFB applications without destroying the persistent LXDE session.

Lease sequence:
1. Save fb_var_screeninfo and HIFB alpha.
2. SIGSTOP Xfbdev-shadow-full. LXDE clients remain alive and blocked on X.
3. Run exactly one native framebuffer application.
4. Restore framebuffer/HIFB state.
5. SIGCONT Xfbdev.
6. Kick a FullFrame shadow redraw through the X software cursor.

APPINFO FORMAT=2 adds BACKEND=native-fb and EXEC_NATIVE.
StayPlaytion is the user-facing identity for the existing RetroArch combiner.
Nofrendo is registered through the same bridge contract.

Input exclusivity is a native-app contract. Existing Nofrendo/RetroArch builds
will be updated to use EVIOCGRAB before Stage6.6C is considered final.
