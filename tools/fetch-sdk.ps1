<#
.SYNOPSIS
    Скачивает официальный foobar2000 SDK и распаковывает его в .\sdk.

.DESCRIPTION
    По умолчанию используется SDK 2023-09-23 - последний SDK эпохи
    foobar2000 v2.0. Архив скачивается с официального сайта foobar2000
    и распаковывается так, чтобы файлы лежали в .\sdk\foobar2000, .\sdk\pfc и т.д.

.PARAMETER Version
    Версия SDK в формате SDK-YYYY-MM-DD.

.PARAMETER Destination
    Каталог назначения (по умолчанию .\sdk).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\fetch-sdk.ps1
#>
[CmdletBinding()]
param(
    [string]$Version = 'SDK-2023-09-23',
    [string]$Destination = (Join-Path $PSScriptRoot '..\sdk')
)

$ErrorActionPreference = 'Stop'

$destination = [System.IO.Path]::GetFullPath($Destination)
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("fb2k-sdk-" + [System.Guid]::NewGuid().ToString('N'))
$archive = Join-Path $tempRoot 'sdk.7z'
$extractDir = Join-Path $tempRoot 'extracted'

New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

$officialUrl = "https://www.foobar2000.org/downloads/$Version.7z"
$usedMirror = $false

Write-Host "Downloading $officialUrl ..."
try {
    Invoke-WebRequest -Uri $officialUrl -OutFile $archive -UseBasicParsing
}
catch {
    Write-Warning "Official download failed: $($_.Exception.Message)"
    Write-Warning "Using the untouched GitHub mirror of $Version instead."
    $mirrorUrl = "https://codeload.github.com/reupen/foobar2000-sdk-unmodified/tar.gz/refs/tags/$Version"
    Invoke-WebRequest -Uri $mirrorUrl -OutFile $archive -UseBasicParsing
    $usedMirror = $true
}

if ($usedMirror) {
    tar -xzf $archive -C $extractDir
    if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }
}
else {
    $sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
    if (-not (Test-Path $sevenZip)) {
        $sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
    }
    if (-not $sevenZip) {
        throw '7-Zip not found. Install 7-Zip (https://www.7-zip.org/) or unpack the SDK manually.'
    }

    & $sevenZip x $archive "-o$extractDir" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed with exit code $LASTEXITCODE" }
}

$candidates = @(Get-Item $extractDir) + @(Get-ChildItem $extractDir -Directory -Recurse)
$sdkRoot = $candidates |
    Where-Object { Test-Path (Join-Path $_.FullName 'foobar2000\SDK\foobar2000.h') } |
    Select-Object -First 1

if (-not $sdkRoot) { throw 'Could not locate the foobar2000 SDK root in the downloaded archive.' }

if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item -Path (Join-Path $sdkRoot.FullName '*') -Destination $destination -Recurse -Force

Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "foobar2000 SDK $Version is ready at $destination"
Get-ChildItem $destination | Select-Object -ExpandProperty Name
