"""Generate redistributable AAC fixtures from a sine wave and digital silence.

Requires the FFmpeg command-line tool only for regeneration, not for tests.
SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import subprocess
import tempfile


def main():
    lines = [
        "// Generated test audio: 440 Hz stereo tone and mono silence, never game data.",
        "// SPDX-License-Identifier: GPL-2.0-or-later",
        "// Regenerate with tools/generate_aac_test_data.py.",
        "#pragma once", "#include <array>", "namespace AacTestData {",
    ]
    with tempfile.TemporaryDirectory(prefix="aac-fixtures-") as directory:
        for label, source, channels, bitrate in (
            ("Tone", "sine=frequency=440:sample_rate=44100:duration=0.10", 2, "96000"),
            ("Mono", "anullsrc=r=48000:cl=mono", 1, "64000"),
        ):
            output = Path(directory) / (label + ".aac")
            subprocess.run([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", source,
                "-t", "0.10", "-ac", str(channels), "-c:a", "aac", "-b:a", bitrate,
                "-f", "adts", "-y", str(output),
            ], check=True)
            data = output.read_bytes()
            lines.append(f"inline constexpr std::array<unsigned char, {len(data)}> {label} = {{")
            for offset in range(0, len(data), 16):
                lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[offset:offset + 16]) + ",")
            lines.append("};")
    lines.append("}")
    Path(__file__).with_name("aac_test_data.h").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
