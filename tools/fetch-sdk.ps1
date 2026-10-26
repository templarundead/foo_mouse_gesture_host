<#
.SYNOPSIS
    Скачивает официальный foobar2000 SDK, проверяет SHA-256 и распаковывает его в .\sdk.

.DESCRIPTION
    Primary source - официальный .7z. Если он недоступен или повреждён, используется
    отдельно закреплённый tar.gz GitHub mirror. Ни один архив не распаковывается
    до успешной проверки своего SHA-256.

.PARAMETER Version
    Поддерживаемая версия SDK. Хеши закреплены отдельно для каждой версии.

.PARAMETER Destination
    Каталог назначения (по умолчанию .\sdk).

.PARAMETER MirrorOnly
    Пропустить официальный источник и использовать проверенный GitHub mirror.
#>
[CmdletBinding()]
param(
    [ValidateSet('SDK-2023-09-23')]
    [string]$Version = 'SDK-2023-09-23',
    [string]$Destination = (Join-Path $PSScriptRoot '..\sdk'),
    [switch]$MirrorOnly
)

$ErrorActionPreference = 'Stop'

$officialUrl = "https://www.foobar2000.org/downloads/$Version.7z"
$officialSha256 = 'c64a9e972a77c4f49a9b67464e5a25f0353cf6758c8a537f8c972e55c93b4364'
$mirrorUrl = "https://codeload.github.com/reupen/foobar2000-sdk-unmodified/tar.gz/refs/tags/$Version"
$mirrorSha256 = '48b8656c98a76c0d856f6e39ad92f333bfcee324ed7774e32fd087bd9c1ccedf'

function Receive-VerifiedArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Write-Host "Downloading $Url ..."
    Invoke-WebRequest -Uri $Url -OutFile $Path -UseBasicParsing
    $actualHash = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ExpectedSha256) {
        throw "SDK SHA-256 mismatch for ${Url}: expected $ExpectedSha256, got $actualHash"
    }
    Write-Host "Verified SDK SHA-256: $actualHash"
}

$destination = [System.IO.Path]::GetFullPath($Destination)
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("fb2k-sdk-" + [System.Guid]::NewGuid().ToString('N'))
$archive = Join-Path $tempRoot 'sdk.archive'
$extractDir = Join-Path $tempRoot 'extracted'

try {
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

    $usedMirror = $MirrorOnly
    if (-not $MirrorOnly) {
        try {
            Receive-VerifiedArchive -Url $officialUrl -ExpectedSha256 $officialSha256 -Path $archive
        }
        catch {
            Write-Warning "Official SDK download or verification failed: $($_.Exception.Message)"
            Write-Warning 'Trying the independently pinned GitHub mirror.'
            $usedMirror = $true
        }
    }

    if ($usedMirror) {
        Receive-VerifiedArchive -Url $mirrorUrl -ExpectedSha256 $mirrorSha256 -Path $archive
        tar -xzf $archive -C $extractDir
        if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }
    }
    else {
        # windows-latest содержит 7-Zip. Не устанавливаем незакреплённые пакеты в hot path.
        $sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
        if (-not (Test-Path $sevenZip)) {
            $command = Get-Command 7z -ErrorAction SilentlyContinue
            if ($command) { $sevenZip = $command.Source }
        }
        if (-not $sevenZip -or -not (Test-Path $sevenZip)) {
            throw '7-Zip is required for the official SDK archive but is not installed.'
        }

        & $sevenZip x $archive "-o$extractDir" -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed with exit code $LASTEXITCODE" }
    }

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

    $source = if ($usedMirror) { 'SHA-256 verified GitHub mirror' } else { 'SHA-256 verified official archive' }
    Write-Host "foobar2000 SDK $Version is ready at $destination"
    Write-Host "::notice title=SDKSource::$Version from $source"
}
finally {
    Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
