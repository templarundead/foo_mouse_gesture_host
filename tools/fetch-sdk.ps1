<#
.SYNOPSIS
    Скачивает и проверяет foobar2000 SDK, затем распаковывает его в .\sdk.

.PARAMETER Version
    Поддерживаемая версия SDK. Хеш закреплён отдельно для каждой версии.

.PARAMETER Destination
    Каталог назначения (по умолчанию .\sdk).

.PARAMETER MirrorOnly
    Использовать только неизменяемый по содержимому GitHub mirror.
#>
[CmdletBinding()]
param(
    [ValidateSet('SDK-2023-09-23')]
    [string]$Version = 'SDK-2023-09-23',
    [string]$Destination = (Join-Path $PSScriptRoot '..\sdk'),
    [switch]$MirrorOnly
)

$ErrorActionPreference = 'Stop'

# Хеш относится именно к tar.gz codeload для этого тега, а не к официальному .7z.
$mirrorSha256 = '48b8656c98a76c0d856f6e39ad92f333bfcee324ed7774e32fd087bd9c1ccedf'
$mirrorUrl = "https://codeload.github.com/reupen/foobar2000-sdk-unmodified/tar.gz/refs/tags/$Version"

$destination = [System.IO.Path]::GetFullPath($Destination)
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("fb2k-sdk-" + [System.Guid]::NewGuid().ToString('N'))
$archive = Join-Path $tempRoot 'sdk.tar.gz'
$extractDir = Join-Path $tempRoot 'extracted'

try {
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

    if (-not $MirrorOnly) {
        throw 'The official SDK source is not yet enabled in this bootstrap revision; use -MirrorOnly.'
    }

    Write-Host "Downloading $mirrorUrl ..."
    Invoke-WebRequest -Uri $mirrorUrl -OutFile $archive -UseBasicParsing

    $actualHash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $mirrorSha256) {
        throw "SDK mirror SHA-256 mismatch: expected $mirrorSha256, got $actualHash"
    }
    Write-Host "Verified SDK mirror SHA-256: $actualHash"

    # Архив распаковывается только после успешной проверки хеша.
    tar -xzf $archive -C $extractDir
    if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }

    $candidates = @(Get-Item $extractDir) + @(Get-ChildItem $extractDir -Directory -Recurse)
    $sdkRoot = $candidates |
        Where-Object { Test-Path (Join-Path $_.FullName 'foobar2000\SDK\foobar2000.h') } |
        Select-Object -First 1

    if (-not $sdkRoot) { throw 'Could not locate the foobar2000 SDK root in the verified archive.' }

    if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -Path (Join-Path $sdkRoot.FullName '*') -Destination $destination -Recurse -Force

    if (-not (Test-Path (Join-Path $destination 'foobar2000\SDK\foobar2000.h'))) {
        throw 'SDK was not copied correctly.'
    }

    Write-Host "foobar2000 SDK $Version is ready at $destination"
    Write-Host "::notice title=SDKSource::$Version from SHA-256 verified GitHub mirror"
}
finally {
    Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
