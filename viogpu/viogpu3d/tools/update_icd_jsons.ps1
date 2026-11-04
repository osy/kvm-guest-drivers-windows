param(
  [string]$MesaPrefix = $env:MESA_PREFIX,
  [string]$OutDir = (Join-Path $PSScriptRoot '..\icd')
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($MesaPrefix)) {
  throw 'Set MESA_PREFIX or pass -MesaPrefix.'
}

$sourceDir = Join-Path $MesaPrefix 'share\vulkan\icd.d'

if (-not (Test-Path $sourceDir)) {
  throw "Missing ICD directory: $sourceDir"
}
if (-not (Test-Path $OutDir)) {
  New-Item -ItemType Directory -Path $OutDir | Out-Null
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

foreach ($src in (Get-ChildItem -Path $sourceDir -Filter '*.json')) {
  $obj = Get-Content -Raw -Path $src.FullName | ConvertFrom-Json
  if (-not $obj.ICD) {
    throw "Invalid ICD JSON (missing ICD object): $($src.FullName)"
  }

  $libraryPath = $obj.ICD.library_path
  $dllPath = Join-Path $sourceDir $libraryPath
  if (-not (Test-Path $dllPath)) {
    throw "Missing ICD DLL: $dllPath"
  }

  # The DLL is staged next to the JSON in the DriverStore.  The Vulkan loader
  # loads a bare file name through the system search path (which does not
  # include the DriverStore; recent loaders fail it with ERROR_INVALID_PARAMETER),
  # and resolves a relative path against the manifest's directory.
  $dllName = Split-Path $libraryPath -Leaf
  $obj.ICD.library_path = '.\' + $dllName
  $json = $obj | ConvertTo-Json -Depth 10

  # Drop Mesa's architecture suffix so the staged name is arch-agnostic
  # (virtio_icd.aarch64.json / virtio_icd.x86_64.json -> virtio_icd.json).
  $dstName = $src.Name -replace '\.[^.]+\.json$', '.json'
  $dst = Join-Path $OutDir $dstName
  [System.IO.File]::WriteAllText($dst, $json, $utf8NoBom)
}

Write-Host "Updated ICD JSONs in $OutDir"
