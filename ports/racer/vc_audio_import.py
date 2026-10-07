#!/usr/bin/env python3
"""
Local-only Vice City audio importer for H3531 Racer.

No Rockstar audio is embedded in the repository or CI artifact. This tool reads
assets from the user's installed PC copy and writes runtime files locally.

RADIO_<STATION>.PCM runtime format:
    signed 16-bit little-endian, mono, 48000 Hz, headerless PCM.

Vice City PC ADF radio files are MP3 byte streams XOR-obfuscated with 0x22.
The script decrypts one or more selected stations and uses a locally installed
ffmpeg to produce RADIO_<STATION>.PCM. Temporary MP3 files are removed after a
successful conversion so several stations do not consume double the USB space.

sfx.sdt entries are 20-byte little-endian records:
    offset, size, sample_rate, loop_start, loop_end
and sfx.raw stores signed 16-bit mono PCM payloads. The importer also exports
selected reference SFX as WAV for inspection/future sample replacement.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

STATION_NAMES = (
    "WILD", "FLASH", "KCHAT", "FEVER", "VROCK",
    "VCPR", "ESPANTOSO", "EMOTION", "WAVE"
)
DEFAULT_STATIONS = ("WAVE", "VROCK", "FEVER", "EMOTION")

# Classic PC releases do not use one perfectly consistent stem naming scheme
# across all distributions. Keep the logical station name stable at runtime and
# resolve the user's local file through these known aliases.
STATION_FILE_ALIASES = {
    "WILD": ("WILD", "WILDSTYLE"),
    "FLASH": ("FLASH",),
    "KCHAT": ("KCHAT",),
    "FEVER": ("FEVER",),
    "VROCK": ("VROCK",),
    "VCPR": ("VCPR", "VPR"),
    "ESPANTOSO": ("ESPANTOSO", "ESPANT", "ESPANTO"),
    "EMOTION": ("EMOTION",),
    "WAVE": ("WAVE",),
}

RADIO_RATE = 24000

REFERENCE_SFX = {
    23: "car_engine_start",
    24: "road_noise",
    25: "tire_skid",
    26: "gravel_skid",
    33: "tyre_bump",
    92: "tarmac_hit",
    101: "car_panel_hit",
    136: "car_collision",
    208: "oceanic_rev9",
    228: "oceanic_idle9",
    339: "oceanic_accel9",
    340: "oceanic_after_accel9",
    341: "oceanic_finger_off_accel9",
}

RUNTIME_SFX = {
    24: "ROAD_NOISE.PCM",
    25: "SKID.PCM",
    26: "GRAVEL_SKID.PCM",
    33: "LANDING.PCM",
    101: "IMPACT.PCM",
    276: "ENGINE_REV.PCM",
    296: "ENGINE_IDLE.PCM",
    407: "ENGINE_ACCEL.PCM",
    408: "ENGINE_CRUISE.PCM",
    409: "ENGINE_RELEASE.PCM",
}

LOOPING_RUNTIME_SFX = {24, 25, 26, 276, 296, 408}


def find_audio_dir(game: Path) -> Path:
    for name in ("Audio", "audio", "AUDIO"):
        p = game / name
        if p.is_dir():
            return p
    raise SystemExit(f"Vice City Audio directory not found under: {game}")


def find_casefold(directory: Path, wanted: str) -> Path | None:
    key = wanted.casefold()
    for p in directory.iterdir():
        if p.name.casefold() == key:
            return p
    return None


def decrypt_adf(src: Path, dst: Path) -> None:
    with src.open("rb") as fi, dst.open("wb") as fo:
        while True:
            chunk = fi.read(1024 * 1024)
            if not chunk:
                break
            fo.write(bytes(b ^ 0x22 for b in chunk))


def build_radio(audio_dir: Path, out: Path, station: str) -> dict:
    station = station.upper()
    if station not in STATION_NAMES:
        raise SystemExit(
            f"Unknown station {station!r}; choose one of: {', '.join(STATION_NAMES)}"
        )

    adf = None
    mp3 = None
    for stem in STATION_FILE_ALIASES.get(station, (station,)):
        if adf is None:
            adf = find_casefold(audio_dir, stem + ".adf")
        if mp3 is None:
            mp3 = find_casefold(audio_dir, stem + ".mp3")

    local_mp3 = out / f"RADIO_{station}.mp3"
    if adf:
        print(f"[audio-import] decrypt {adf.name} -> {local_mp3.name}")
        decrypt_adf(adf, local_mp3)
        source = str(adf)
    elif mp3:
        print(f"[audio-import] copy {mp3.name} -> {local_mp3.name}")
        shutil.copyfile(mp3, local_mp3)
        source = str(mp3)
    else:
        return {
            "station": station,
            "source": None,
            "radio_pcm": False,
            "reason": "station file not found",
        }

    ffmpeg = shutil.which("ffmpeg")
    mulaw = out / f"RADIO_{station}.MULAW"
    legacy_pcm = out / f"RADIO_{station}.PCM"
    if not ffmpeg:
        print(
            f"[audio-import] ffmpeg not found; {local_mp3.name} was prepared, "
            f"but {mulaw.name} was not built."
        )
        return {
            "station": station,
            "source": source,
            "radio_mp3": str(local_mp3),
            "radio_ready": False,
            "reason": "ffmpeg not found on PATH",
        }

    cmd = [
        ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
        "-i", str(local_mp3),
        "-vn", "-ac", "1", "-ar", str(RADIO_RATE),
        "-f", "mulaw", "-acodec", "pcm_mulaw", str(mulaw),
    ]
    print(
        f"[audio-import] transcode {station} -> "
        f"{RADIO_RATE//1000}kHz mono G.711 mu-law"
    )
    subprocess.run(cmd, check=True)
    try:
        local_mp3.unlink()
    except OSError:
        pass

    # A successful compact build supersedes the old enormous 48 kHz S16 file.
    # Remove only this station's legacy output after the new file exists.
    if mulaw.exists() and mulaw.stat().st_size > 0 and legacy_pcm.exists():
        try:
            legacy_pcm.unlink()
            print(f"[audio-import] removed legacy {legacy_pcm.name}")
        except OSError as exc:
            print(f"[audio-import] warning: could not remove {legacy_pcm.name}: {exc}")

    return {
        "station": station,
        "source": source,
        "radio_ready": True,
        "radio_format": "mulaw",
        "radio_rate": RADIO_RATE,
        "radio_path": str(mulaw),
        "radio_bytes": mulaw.stat().st_size,
    }


def resample_s16_mono(data: bytes, src_rate: int, dst_rate: int = 48000) -> bytes:
    if src_rate <= 0:
        raise ValueError(f"invalid sample rate {src_rate}")
    if len(data) & 1:
        data = data[:-1]
    samples = list(struct.unpack("<" + "h" * (len(data) // 2), data))
    if not samples:
        return b""
    if src_rate == dst_rate:
        return data
    out_count = max(1, int(round(len(samples) * dst_rate / src_rate)))
    out = [0] * out_count
    scale = src_rate / dst_rate
    last = len(samples) - 1
    for i in range(out_count):
        pos = i * scale
        j = int(pos)
        if j >= last:
            out[i] = samples[last]
        else:
            frac = pos - j
            v = samples[j] + (samples[j + 1] - samples[j]) * frac
            out[i] = max(-32768, min(32767, int(round(v))))
    return struct.pack("<" + "h" * len(out), *out)


def read_sdt(path: Path) -> list[tuple[int, int, int, int, int]]:
    raw = path.read_bytes()
    if len(raw) % 20:
        raise SystemExit(f"Invalid sfx.sdt size {len(raw)} (not divisible by 20)")
    return [
        struct.unpack_from("<IIIII", raw, off)
        for off in range(0, len(raw), 20)
    ]


def export_reference_sfx(audio_dir: Path, out: Path) -> dict:
    sdt = find_casefold(audio_dir, "sfx.sdt")
    raw = find_casefold(audio_dir, "sfx.raw")
    if not sdt or not raw:
        return {"available": False, "reason": "sfx.sdt/sfx.raw not found"}

    entries = read_sdt(sdt)
    result: dict[str, object] = {
        "available": True,
        "entry_count": len(entries),
        "exports": [],
    }

    with raw.open("rb") as fp:
        for idx, label in REFERENCE_SFX.items():
            if idx >= len(entries):
                continue
            offset, size, rate, loop_start, loop_end = entries[idx]
            fp.seek(offset)
            pcm = fp.read(size)
            if len(pcm) != size:
                raise SystemExit(
                    f"sfx.raw truncated for id={idx}: need={size} got={len(pcm)}"
                )
            wav_path = out / f"SFX{idx:05d}_{label}.wav"
            with wave.open(str(wav_path), "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(rate)
                wf.writeframes(pcm)
            result["exports"].append(
                {
                    "id": idx,
                    "label": label,
                    "sample_rate": rate,
                    "bytes": size,
                    "loop_start": loop_start,
                    "loop_end": loop_end,
                    "path": str(wav_path),
                }
            )
            runtime_name = RUNTIME_SFX.get(idx)
            if runtime_name:
                runtime_path = out / runtime_name

                # For looping SFX, export the authored SDT loop window rather
                # than the whole sample. This is especially important for
                # tyre skid: the source contains an attack before loop_start;
                # replaying that attack every cycle sounded like repeated
                # short skids instead of one continuous slide.
                runtime_src = pcm
                runtime_loop = False
                ls = int(loop_start)
                le = int(loop_end)
                if (
                    idx in LOOPING_RUNTIME_SFX
                    and ls != 0xFFFFFFFF
                    and le != 0xFFFFFFFF
                    and 0 <= ls < le <= len(pcm)
                    and (le - ls) >= 64
                ):
                    ls &= ~1
                    le &= ~1
                    runtime_src = pcm[ls:le]
                    runtime_loop = True

                runtime_pcm = resample_s16_mono(runtime_src, rate, 48000)
                runtime_path.write_bytes(runtime_pcm)
                result["exports"][-1]["runtime_pcm"] = str(runtime_path)
                result["exports"][-1]["runtime_rate"] = 48000
                result["exports"][-1]["runtime_bytes"] = len(runtime_pcm)
                result["exports"][-1]["runtime_loop_window"] = runtime_loop
                if runtime_loop:
                    result["exports"][-1]["runtime_loop_source_bytes"] = [ls, le]
            print(
                f"[audio-import] sfx id={idx} {label} rate={rate} bytes={size}"
                + (f" -> {runtime_name}" if runtime_name else "")
            )
    return result


def normalize_stations(values: list[str]) -> list[str]:
    flat: list[str] = []
    for value in values:
        for token in value.replace(",", " ").split():
            name = token.upper()
            if name == "ALL":
                flat.extend(STATION_NAMES)
            else:
                flat.append(name)
    if not flat:
        flat.extend(DEFAULT_STATIONS)

    result: list[str] = []
    for name in flat:
        if name not in STATION_NAMES:
            raise SystemExit(
                f"Unknown station {name!r}; choose: {', '.join(STATION_NAMES)} or ALL"
            )
        if name not in result:
            result.append(name)
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "game",
        type=Path,
        help=r'Vice City install root, e.g. "E:\Games\GTA Vice City"',
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=Path(r"E:\H3531\APPS\racer\audio"),
        help="Racer audio output directory",
    )
    ap.add_argument(
        "--stations",
        nargs="*",
        default=list(DEFAULT_STATIONS),
        help=(
            "Stations to import. Default: WAVE VROCK FEVER EMOTION. "
            "Use ALL for all stations; comma-separated names are accepted."
        ),
    )
    args = ap.parse_args()

    game = args.game.resolve()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    audio_dir = find_audio_dir(game)

    stations = normalize_stations(args.stations)
    radio = [build_radio(audio_dir, out, station) for station in stations]
    manifest = {
        "format": "h3531-racer-audio-v3",
        "game": str(game),
        "audio_dir": str(audio_dir),
        "stations_requested": stations,
        "radio": radio,
        "sfx": export_reference_sfx(audio_dir, out),
    }
    manifest_path = out / "AUDIO_MANIFEST.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"[audio-import] manifest: {manifest_path}")
    ready = [r["station"] for r in radio if r.get("radio_ready")]
    print(
        "[audio-import] runtime stations ready: "
        + (", ".join(ready) if ready else "NONE")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
