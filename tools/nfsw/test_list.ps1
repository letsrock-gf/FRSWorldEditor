# Isolated test (build-test copy, scratch folder) of a save that starts from an edit list: every image of
# -Textures but the last goes into the list the editor reads at load, the last is replaced after loading
# (FRS_TEST_TEXTURE) and the save follows. Sections whose textures came from the list must be written too,
# or the section table loses what they grew by. verify_resize.py reads the result.
param([string]$Out, [string]$Textures, [string]$List)
$d = 'D:\NFS World\work\nfsw-world-editor\build-test\bin\Release'
$log = "$d\FRSWorldEditor.log"
$images = @(Get-ChildItem $Textures -File | Sort-Object Name)
if ($images.Count -lt 2) { "need two images or more in $Textures"; return }

Get-Process FRSWorldEditor -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d*" } | Stop-Process -Force
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force "$Out\frs-original\textures" | Out-Null
$last = $images[$images.Count - 1]
# With -List the image replaced after loading is the one of the list's last T line: the folder may also hold
# images of other kinds of line, which FRS_TEST_TEXTURE cannot stand for.
if ($List) {
    $named = (@(Get-Content $List | Where-Object { $_ -match '^T ' })[-1] -split ' ', 3)[2]
    $last = $images | Where-Object { $_.Name -eq $named } | Select-Object -First 1
    if (-not $last) { "the image $named of the list's last T line is not in $Textures"; return }
}
$lines = ''
foreach ($image in $images) {
    if ($image.FullName -eq $last.FullName) { continue }
    Copy-Item $image.FullName "$Out\frs-original\textures\$($image.Name)"
    $lines += "T $($image.BaseName) $($image.Name)`n"
}
# -List <file>: that list as it is (removed and moved objects too), with the last image's line taken out.
if ($List) { $lines = ((Get-Content $List | Where-Object { $_ -notmatch "^T $($last.BaseName) " }) -join "`n") + "`n" }
[IO.File]::WriteAllText("$Out\frs-original\L5RA-edits.txt", $lines)
# Edited models named by the list are kept beside it.
if ($List -and (Test-Path "$(Split-Path $List)\meshes")) { Copy-Item "$(Split-Path $List)\meshes" "$Out\frs-original\meshes" -Recurse }

Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:FRS_TEST_EXPORT -ErrorAction SilentlyContinue
$env:FRS_WRITE_TRACKS = $Out
$env:FRS_TEST_SAVE = $Out
$env:FRS_TEST_TEXTURE = "$($last.BaseName);$($last.FullName)"
$p = Start-Process "$d\FRSWorldEditor.exe" -WorkingDirectory $d -PassThru
$deadline = (Get-Date).AddMinutes(8)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'NFS World save|Could not' -Quiet)) { break }
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 2
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Select-String -Path $log -Pattern 'NFS World edits|NFS World save|\[error\]' | ForEach-Object { $_.Line }
"staged:  $((Select-String -Path $log -Pattern 'texture [0-9A-F]{8}: image').Count) textures"
"written: $((Select-String -Path $log -Pattern 'NFS World: section .* written').Count) sections"
"list:    $((Get-Content "$Out\frs-original\L5RA-edits.txt" | Measure-Object -Line).Lines) lines"
