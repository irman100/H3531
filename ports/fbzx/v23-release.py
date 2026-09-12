from pathlib import Path
import sys


def replace_braced_function(text: str, signature: str, replacement: str) -> str:
    pos = text.find(signature)
    if pos < 0:
        raise SystemExit(f"v23: function signature missing: {signature}")
    brace = text.find("{", pos)
    if brace < 0:
        raise SystemExit(f"v23: opening brace missing: {signature}")
    depth = 0
    i = brace
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[:pos] + replacement + text[i + 1:]
        i += 1
    raise SystemExit(f"v23: closing brace missing: {signature}")


def prep_repo_sources() -> None:
    # Release runtime: do not create/truncate /var/fbzx.log and do not redirect
    # stdout/stderr. Fatal-signal reporting still writes directly to inherited stderr.
    p = Path("ports/fbzx/h3531-runtime.c")
    s = p.read_text()
    old = "    prepare_session_log();\n    prepare_runtime();"
    new = "    /* release-v23: no session log collection/redirection */\n    prepare_runtime();"
    if s.count(old) != 1:
        raise SystemExit(f"v23: runtime log-call marker count={s.count(old)}")
    s = s.replace(old, new, 1)
    old = '"H3531 runtime prepared; v17-silent log-I/O control + v16 fast16 + v14 AO/frame-clock enabled\\n";'
    new = '"H3531 runtime prepared; release-v23 true-dirty + classic128 + buffered AO; periodic diagnostics disabled\\n";'
    if s.count(old) != 1:
        raise SystemExit(f"v23: runtime identity marker count={s.count(old)}")
    p.write_text(s)

    # Release audio: keep the buffered worker, exact 48 kHz production and the
    # frame clock. Remove per-SendFrame timing, periodic FPS/deadline telemetry,
    # queue counters and shutdown statistics.
    p = Path("ports/fbzx/h3531-audio.cpp")
    s = p.read_text()

    send_pcm = r'''static int send_pcm_block(int16_t *pcm)
{
    H3531AudioFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.enBitwidth = H3531_AUDIO_BIT_WIDTH_16;
    frame.enSoundmode = H3531_AUDIO_SOUND_MODE_MONO;
    frame.pVirAddr[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pcm));
    frame.u32Seq = g_seq++;
    frame.u32Len = kBytesPerAoBlock;
    return ao_ioctl(kIoctlSendFrame, &frame);
}'''
    s = replace_braced_function(s, "static int send_pcm_block(int16_t *pcm)", send_pcm)

    # These functions existed only for diagnostics; remove them entirely.
    s = replace_braced_function(s, "static void reset_perf_window()", "")
    s = replace_braced_function(s, "static void print_perf_window()", "")

    pace = r'''extern "C" void h3531_audio_pace_frame(int turbo)
{
    const uint64_t now = monotonic_ns();
    if (now == 0)
        return;

    if (turbo) {
        g_frame_clock_valid = 0;
        return;
    }

    if (!g_frame_clock_valid) {
        g_frame_deadline_ns = now + kFramePeriodNs;
        g_frame_clock_valid = 1;
        return;
    }

    if (now + 2ULL * kFramePeriodNs < g_frame_deadline_ns ||
        now > g_frame_deadline_ns + 2ULL * kFramePeriodNs) {
        g_frame_deadline_ns = now + kFramePeriodNs;
    } else {
        if (now < g_frame_deadline_ns)
            sleep_until_ns(g_frame_deadline_ns);
        g_frame_deadline_ns += kFramePeriodNs;
    }
}'''
    s = replace_braced_function(s, 'extern "C" void h3531_audio_pace_frame(int turbo)', pace)

    stop = r'''extern "C" void h3531_audio_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);

    if (g_worker_started) {
        pthread_join(g_worker, 0);
        g_worker_started = 0;
    }

    if (g_fd >= 0)
        disable_ao();

    g_active = 0;
    g_frame_clock_valid = 0;
    g_producer_fill = 0;
}'''
    s = replace_braced_function(s, 'extern "C" void h3531_audio_stop(void)', stop)

    # Remove hot-path counter increments and one-time resets that are no longer used.
    for old in (
        "            ++g_queue_empty_waits;\n",
        "            ++g_overruns;\n",
        "    ++g_batches_queued;\n",
        "    ++g_submit_calls;\n",
        "    reset_perf_window();\n",
    ):
        s = s.replace(old, "")

    for old in (
        "    g_blocks_sent = 0;\n",
        "    g_queue_empty_waits = 0;\n",
        "    g_overruns = 0;\n",
        "    g_batches_queued = 0;\n",
        "    g_submit_calls = 0;\n",
        "    g_send_over_1ms = 0;\n",
        "    g_send_over_3ms = 0;\n",
        "    g_send_over_5ms = 0;\n",
        "    g_send_max_us = 0;\n",
        "    g_last_send_rc = 0;\n",
    ):
        s = s.replace(old, "")

    p.write_text(s)

    # Make the temporary build apply the v21/v22 functional patches before the
    # H3531 integration and remove do_flip timing after integration but before make.
    p = Path("ports/fbzx/build-h3531.sh")
    s = p.read_text()
    anchor = 'git apply "$FBZX_PATCH"\n'
    if s.count(anchor) != 1:
        raise SystemExit(f"v23: build pre-upstream anchor count={s.count(anchor)}")
    s = s.replace(anchor, anchor + 'python3 "$SCRIPT_DIR/v23-release.py" pre-upstream\n', 1)
    anchor = 'echo "== Build static FBZX H3531 buffered-audio validation binary =="\n'
    if s.count(anchor) != 1:
        raise SystemExit(f"v23: build post-upstream anchor count={s.count(anchor)}")
    s = s.replace(anchor, 'python3 "$SCRIPT_DIR/v23-release.py" post-upstream\n\n' + anchor, 1)
    p.write_text(s)
    print("H3531 v23: release runtime/audio prepared; session logging disabled")


def pre_upstream() -> None:
    # v21 changed-pixel dirty-row producer hook.
    p = Path("src/llscreen.cpp")
    t = p.read_text()
    inc = '#include "osd.hh"\n'
    if t.count(inc) != 1:
        raise SystemExit("v23/v21: llscreen include marker missing")
    t = t.replace(inc, inc + '\nextern "C" void H3531_FBZX_MarkDirtyAddress(const void *address);\n', 1)

    old = '''\tcase 3:\n\t\t*(address++)=*(colour++);\n\tcase 2:\n\t\t*(address++)=*(colour++);\n\t\t*(address++)=*(colour++);\n\tbreak;'''
    new = '''\tcase 3:\n\t\t*(address++)=*(colour++);\n\t\t*(address++)=*(colour++);\n\t\t*(address++)=*(colour++);\n\tbreak;\n\tcase 2: {\n\t\tUint16 value16 = *((Uint16 *)colour);\n\t\tUint16 *dst16 = (Uint16 *)address;\n\t\tif (*dst16 != value16) {\n\t\t\t*dst16 = value16;\n\t\t\tH3531_FBZX_MarkDirtyAddress(address);\n\t\t}\n\tbreak;\n\t}'''
    if t.count(old) != 1:
        raise SystemExit("v23/v21: paint_one_pixel marker missing")
    t = t.replace(old, new, 1)
    p.write_text(t)

    # v22 classic Spectrum 128K ROM selection.
    p = Path("src/cargador.cpp")
    s = p.read_text()
    old = '''\tcase 1: // 128k\n\t\tprintf("Mode 128K\\n");\n\t\tordenador->mode128k = 2; // +2 mode\n\t\tordenador->issue = 3;\n\t\tResetComputer();'''
    new = '''\tcase 1: // 128k\n\t\tprintf("Mode 128K\\n");\n\t\tprintf("H3531 model: classic 128K ROM set\\n");\n\t\tordenador->mode128k = 1; // classic 128K mode\n\t\tordenador->issue = 3;\n\t\tResetComputer();'''
    if s.count(old) != 1:
        raise SystemExit(f"v23/v22: classic128 marker count={s.count(old)}")
    p.write_text(s.replace(old, new, 1))
    print("H3531 v23: v21 true-dirty producer + v22 classic128 applied")


def post_upstream() -> None:
    # The integration patch deliberately adds v15 clock_gettime diagnostics to
    # every flip. Release restores the simple flip path while keeping pacing in
    # Screen::show_screen and all functional H3531 integration.
    p = Path("src/llscreen.cpp")
    t = p.read_text()
    simple = r'''void LLScreen::do_flip() {

\tif (this->mustlock) {
\t\tSDL_UnlockSurface (this->llscreen);
\t\tSDL_Flip(this->llscreen);
\t\tSDL_LockSurface(this->llscreen);
\t} else {
\t\tSDL_Flip(this->llscreen);
\t}
}'''.replace('\\t', '\t')
    t = replace_braced_function(t, "void LLScreen::do_flip()", simple)
    p.write_text(t)
    print("H3531 v23: per-frame video15 timing instrumentation removed")


if len(sys.argv) != 2:
    raise SystemExit("usage: v23-release.py prep|pre-upstream|post-upstream")

stage = sys.argv[1]
if stage == "prep":
    prep_repo_sources()
elif stage == "pre-upstream":
    pre_upstream()
elif stage == "post-upstream":
    post_upstream()
else:
    raise SystemExit(f"unknown v23 stage: {stage}")
