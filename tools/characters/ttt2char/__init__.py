"""Turns a fighter from the player's own copy of another Tekken game into a character pack for Tekken Tag
Tournament 2 (this repository's port). Everything is read from the player's files at import time; nothing of
any game is stored in this package.

    ttt2/       the Wii U game's archives and file formats (models, textures, skeletons, item databases,
                movesets, motions)
    tekken7/    Tekken 7 PC: pak files, Unreal Engine 4.14 packages (textures, skeletal meshes, materials) and
                TKMovesets2 moveset files
    convert/    Tekken 7 -> TTT2: skeleton, model, textures, item database, moveset
    characters/ what to take for each fighter (asset paths, names and rules, no data)
    pack.py     writes the pack the port loads (runtime/src/title/roster.cpp)
"""


class InputError(Exception):
    """An input is missing or is not what the importer needs; the message says what to do."""
