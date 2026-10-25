"""The Wii U game's archives and file formats, read from the player's installed game."""
import os


def title_folder(folder):
    """The title folder (code/, content/, meta/) of an installed game: `folder` itself, or the game/ folder of an
    install folder (ttt2 --install puts the title there); None when neither holds the game's archives."""
    for title in (folder, os.path.join(folder, 'game')) if folder else ():
        if os.path.isfile(os.path.join(title, 'content', 'hdd', 'data000.ofs')):
            return title
    return None
