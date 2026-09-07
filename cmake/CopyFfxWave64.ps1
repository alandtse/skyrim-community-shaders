param(
    [Parameter(Mandatory = $true)]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$sourcePath = [IO.Path]::GetFullPath($Source)
$destinationPath = [IO.Path]::GetFullPath($Destination)
$sourceDirectory = Split-Path -Parent $sourcePath
$destinationDirectory = Split-Path -Parent $destinationPath

$sourceWrapperName = Split-Path -Leaf $sourcePath
$destinationWrapperName = Split-Path -Leaf $destinationPath
$oldBase = [IO.Path]::GetFileNameWithoutExtension($sourceWrapperName) -replace '_permutations$', ''
$newBase = [IO.Path]::GetFileNameWithoutExtension($destinationWrapperName) -replace '_permutations$', ''

[IO.Directory]::CreateDirectory($destinationDirectory) | Out-Null

$wrapperText = [IO.File]::ReadAllText($sourcePath)
$includeMatches = [regex]::Matches($wrapperText, '#include\s+"([^"]+)"')
if ($includeMatches.Count -eq 0) {
    throw "No generated blob include found in $sourcePath"
}

foreach ($includeMatch in $includeMatches) {
    $sourceBlobName = $includeMatch.Groups[1].Value
    $sourceBlobPath = Join-Path $sourceDirectory $sourceBlobName
    if (-not (Test-Path -LiteralPath $sourceBlobPath)) {
        throw "Generated blob not found: $sourceBlobPath"
    }

    # MSVC still has a MAX_PATH-sensitive include path here. Keep the copied
    # blob filename short while renaming the symbols inside it below.
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $blobId = ([BitConverter]::ToString($sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($sourceBlobName))) -replace '-', '').Substring(0, 16).ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
    $destinationBlobName = "ffx_wave64_blob_$blobId.h"
    $destinationBlobPath = Join-Path $destinationDirectory $destinationBlobName
    $blobText = [IO.File]::ReadAllText($sourceBlobPath).Replace($oldBase, $newBase)
    [IO.File]::WriteAllText($destinationBlobPath, $blobText)
    $wrapperText = $wrapperText.Replace(('"' + $sourceBlobName + '"'), ('"' + $destinationBlobName + '"'))
}

$wrapperText = $wrapperText.Replace($oldBase, $newBase)

[IO.File]::WriteAllText($destinationPath, $wrapperText)
