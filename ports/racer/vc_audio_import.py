#!/usr/bin/env python3
"""
Local-only Vice City audio importer for H3531 Racer.

No Rockstar audio is embedded in the repository or CI artifact. This tool reads
assets from the user's installed PC copy and writes runtime files locally.

RADIO0.PCM runtime format:
    signed 16-bit little-endian, mono, 48000 Hz, headerless PCM.

Vice City PC ADF radio files are MP3 byte streams XOR-obfuscated with 0x22.
The script decrypts the selected station and uses a locally installed ffmpeg
when available to produce RADIO0.PCM. If ffmpeg is absent, RADIO0.mp3 is kept
so conversion can be performed later without re-reading the ADF.

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
    "WAVE", "WILD", "KCHAT", "FEVER", "VROCK", "VCPR", "ESPANTOSO", "EMOTION"
)

REFERENCE_SFX = {
    23: "car_engine_start",
    25: "tire_skid",
}


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

    adf = find_casefold(audio_dir, station + ".adf")
    mp3 = find_casefold(audio_dir, station + ".mp3")

    local_mp3 = out / "RADIO0.mp3"
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
    pcm = out / "RADIO0.PCM"
    if not ffmpeg:
        print(
            "[audio-import] ffmpeg not found; RADIO0.mp3 was prepared, "
            "but RADIO0.PCM was not built."
        )
        return {
            "station": station,
            "source": source,
            "radio_mp3": str(local_mp3),
            "radio_pcm": False,
            "reason": "ffmpeg not found on PATH",
        }

    cmd = [
        ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
        "-i", str(local_mp3),
        "-vn", "-ac", "1", "-ar", "48000",
        "-f", "s16le", "-acodec", "pcm_s16le", str(pcm),
    ]
    print("[audio-import] transcode radio -> 48kHz S16 mono PCM")
    subprocess.run(cmd, check=True)
    return {
        "station": station,
        "source": source,
        "radio_mp3": str(local_mp3),
        "radio_pcm": True,
        "radio_pcm_path": str(pcm),
        "radio_pcm_bytes": pcm.stat().st_size,
    }


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
            print(
                f"[audio-import] sfx id={idx} {label} rate={rate} bytes={size}"
            )
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
        "--station",
        default="WAVE",
        help="One VC radio station to expose as RADIO0 (default: WAVE)",
    )
    args = ap.parse_args()

    game = args.game.resolve()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    audio_dir = find_audio_dir(game)

    manifest = {
        "format": "h3531-racer-audio-v1",
        "game": str(game),
        "audio_dir": str(audio_dir),
        "radio": build_radio(audio_dir, out, args.station),
        "sfx": export_reference_sfx(audio_dir, out),
    }
    manifest_path = out / "AUDIO_MANIFEST.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"[audio-import] manifest: {manifest_path}")
    print(
        "[audio-import] runtime radio: "
        + ("READY" if manifest["radio"].get("radio_pcm") else "NOT READY")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
