[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
    [ValidateSet('split', 'ordered')]
    [string]$Variant = 'split'
)

$ErrorActionPreference = 'Stop'

$donorRoot = Join-Path $RepoRoot 'cod2map-master\cod2src'
if (-not (Test-Path -LiteralPath $donorRoot)) {
    throw "CoD2 donor source was not found at $donorRoot"
}

$vswhereCandidates = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe',
    'C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe'
)
$vswhere = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $vswhere) {
    throw 'vswhere.exe was not found'
}

$vsRoot = & $vswhere -latest -property installationPath
if (-not $vsRoot) {
    throw 'Visual Studio was not found'
}
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars32.bat'

$outputRoot = Join-Path $RepoRoot "build\cod4map-obj-split\donor-$Variant"
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$sources = Get-ChildItem -LiteralPath $donorRoot -Recurse -File -Filter '*.c' |
    Where-Object { $_.FullName -notmatch '\\libs\\(?:zlib|minizip)\\' } |
    Sort-Object FullName

if ($sources.Count -ne 41) {
    throw "Expected 41 first-party donor TUs, found $($sources.Count)"
}

$functionSections = if ($Variant -eq 'split') { '/Gy' } else { '/Gy-' }
$commonFlags = @(
    '/nologo',
    '/c',
    '/O2',
    '/Ot',
    '/Ob2',
    '/Oi',
    '/Oy',
    '/GS-',
    '/fp:precise',
    '/arch:IA32',
    '/MT',
    '/W0',
    '/Z7',
    '/DDETERMINISTIC_SORT',
    $functionSections,
    "/I`"$(Join-Path $PSScriptRoot 'include')`"",
    "/I`"$donorRoot`"",
    "/I`"$(Join-Path $donorRoot 'libs\zlib')`"",
    "/I`"$(Join-Path $donorRoot 'libs\minizip')`""
)

foreach ($source in $sources) {
    $relative = $source.FullName.Substring($donorRoot.Length + 1)
    $objectName = ($relative -replace '[\\/]', '__') -replace '\.c$', '.obj'
    $objectPath = Join-Path $outputRoot $objectName
    $arguments = @($commonFlags) + @(
        "/Fo`"$objectPath`"",
        "`"$($source.FullName)`""
    )
    $command = "call `"$vcvars`" >nul && cl " + ($arguments -join ' ')
    Write-Host "[$Variant] $relative"
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed for $relative (exit $LASTEXITCODE)"
    }
}

$built = Get-ChildItem -LiteralPath $outputRoot -File -Filter '*.obj'
if ($built.Count -ne $sources.Count) {
    throw "Expected $($sources.Count) objects, found $($built.Count)"
}

Write-Host "Built $($built.Count) donor objects in $outputRoot"
