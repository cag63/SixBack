# Exclude Spotify-only assets from the ESP32-classic LittleFS image.
#
# The classic (env:esp32) uses partitions-4mb.csv with a 256 KB spiffs slot and
# has no Spotify (SIXBACK_SPOTIFY_ENABLED is unset for this env), so silence.mp3
# (~120 KB, only used by the Spotify TUNEIN-tunnel) is dead weight that pushes
# the data/ payload past the partition -> LFS_ERR_NOSPC at buildfs time.
#
# Instead of editing the shared data/ dir, we point PROJECT_DATA_DIR at a
# filtered staging copy for this env only. Other envs (s3/c3/c6) keep the full
# data/ (they enable Spotify and have a larger spiffs).
import os
import shutil

Import("env")  # noqa: F821

EXCLUDE = {"silence.mp3"}

src = env.subst("$PROJECT_DATA_DIR")
staging = os.path.join(env.subst("$BUILD_DIR"), "fs_data")

if os.path.isdir(src):
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    shutil.copytree(
        src, staging,
        ignore=lambda _d, names: [n for n in names if n in EXCLUDE],
    )
    env.Replace(PROJECT_DATA_DIR=staging)
    print("fs_exclude_esp32: FS image source -> %s (excluded: %s)"
          % (staging, ", ".join(sorted(EXCLUDE))))
