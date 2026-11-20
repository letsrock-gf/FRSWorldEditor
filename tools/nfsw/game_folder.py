"""The Need for Speed World folder the measuring scripts read: FRS_GAME_FOLDER when it is set, else a folder
named Data2015 two folders above the repository (the layout the port was made in)."""
import os

_here = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("FRS_GAME_FOLDER") or os.path.normpath(os.path.join(_here, "..", "..", "..", "..", "Data2015"))
TRACKS = os.path.join(GAME, "TRACKS")
COLLISION = os.path.join(TRACKS, "STREAML5RA_25000.BUN")
