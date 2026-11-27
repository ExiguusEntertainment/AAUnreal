"""Import the original level music in Unreal Editor.

Render MUSIC1.HMI through MUSIC6.HMI from Exe and L1.HMI from
Resources/ArtFinal/MUSIC with libADLMIDI 1.6.2. Use
`adlmidiplay MUSIC1.HMI -w -nl -vm 10 2` for each track and put the rendered
files in UNREALAA_WAD_MUSIC_DIR. Run this script in the Editor, then
run AA.RefreshWadItemRules to link the sound waves to the authored maps.
"""

import os
from pathlib import Path

import unreal


def import_music():
    source = Path(os.environ["UNREALAA_WAD_MUSIC_DIR"])
    tasks = []
    for name in [f"MUSIC{index}" for index in range(1, 7)] + ["L1"]:
        path = source / f"{name}.HMI.wav"
        if not path.is_file():
            raise RuntimeError(f"Missing rendered HMI track {path}")
        task = unreal.AssetImportTask()
        task.filename = str(path)
        task.destination_path = "/Game/WadInterface/Music"
        task.destination_name = name
        task.automated = True
        task.replace_existing = True
        tasks.append(task)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    for task in tasks:
        assets = task.get_objects()
        if len(assets) != 1 or not isinstance(assets[0], unreal.SoundWave):
            raise RuntimeError(f"Failed to import {task.filename} as a sound wave")
        wave = assets[0]
        wave.set_editor_property("looping", True)
        if not unreal.EditorAssetLibrary.save_loaded_asset(wave):
            raise RuntimeError(f"Could not save {wave.get_path_name()}")

    unreal.log("Imported all seven authored WAD music tracks.")


import_music()
