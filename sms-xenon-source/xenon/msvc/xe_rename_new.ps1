# The game replaces the global operator new/delete with JKRHeap allocation. On the PC the
# build renames the game's definitions and every game reference with objcopy (CMakeLists.txt),
# so the CRT and the port's own code keep the C runtime's. This does the same to the game's
# COFF objects: each mangled name is replaced in place by one of the same length (last
# letter Z -> Y), in the string table and wherever else the bytes occur.
param([string]$List)
$latin1 = [Text.Encoding]::GetEncoding(28591)
$pairs = @(
  @('??2@YAPAXI@Z',  '??2@YAPAXI@Y'),   # operator new(size_t)
  @('??_U@YAPAXI@Z', '??_U@YAPAXI@Y'),  # operator new[](size_t)
  @('??3@YAXPAX@Z',  '??3@YAXPAX@Y'),   # operator delete(void*)
  @('??_V@YAXPAX@Z', '??_V@YAXPAX@Y')   # operator delete[](void*)
)
$n = 0
foreach ($obj in Get-Content $List) {
  if (-not (Test-Path $obj)) { Write-Error "xe_rename_new: missing $obj"; exit 1 }
  $bytes = [IO.File]::ReadAllBytes($obj)
  $text = $latin1.GetString($bytes)
  $new = $text
  foreach ($p in $pairs) { $new = $new.Replace($p[0], $p[1]) }
  if ($new -ne $text) { [IO.File]::WriteAllBytes($obj, $latin1.GetBytes($new)); $n++ }
}
Write-Output "xe_rename_new: $n game objects renamed"
