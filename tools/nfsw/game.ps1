# The Need for Speed World folder the test scripts read: FRS_GAME_FOLDER when it is set, else a folder named
# Data2015 two folders above the repository (the layout the port was made in). The scripts only read it: every
# write goes to the scratch folder a script is given (FRS_WRITE_TRACKS).
if ($env:FRS_GAME_FOLDER) { $env:FRS_GAME_FOLDER }
else { Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot)))) 'Data2015' }
