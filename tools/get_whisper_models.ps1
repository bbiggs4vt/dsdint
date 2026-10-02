<#
.SYNOPSIS
  Download Whisper speech-to-text models for the browser (Transformers.js /
  ONNX) from Hugging Face, without the Hugging Face command-line tool.

.DESCRIPTION
  Fetches only what Transformers.js needs: the small config / tokenizer files
  and the two quantized ONNX files -- not the full-precision copies (hundreds
  of MB) the repos also hold. Re-running skips files already complete. The
  large files are checked against the SHA-256 Hugging Face publishes for them.
  Works in Windows PowerShell 5.1 and PowerShell 7+.

.EXAMPLE
  .\get_whisper_models.ps1
  # Xenova/whisper-base.en and Xenova/whisper-small.en -> .\whisper-models, plus whisper-models.zip

.EXAMPLE
  .\get_whisper_models.ps1 -Models Xenova/whisper-base.en -NoZip

.NOTES
  If scripts are blocked:  powershell -ExecutionPolicy Bypass -File .\get_whisper_models.ps1
  HF_ENDPOINT (env) overrides https://huggingface.co; HF_TOKEN (env) is only
  needed for gated / private repos.
#>
param(
    [string[]] $Models = @('Xenova/whisper-base.en', 'Xenova/whisper-small.en'),
    [string]   $Out = 'whisper-models',
    [switch]   $NoZip
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'      # Windows PowerShell's progress bar slows downloads a lot
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$HF = 'https://huggingface.co'
if ($env:HF_ENDPOINT) { $HF = $env:HF_ENDPOINT.TrimEnd('/') }
$Headers = @{}
if ($env:HF_TOKEN) { $Headers['Authorization'] = "Bearer $($env:HF_TOKEN)" }
$WantOnnx = @('onnx/encoder_model_quantized.onnx', 'onnx/decoder_model_merged_quantized.onnx')
$Total = [int64]0

foreach ($repo in $Models) {
    $name = ($repo -split '/')[-1]
    Write-Host "== $repo"
    try {
        $info = Invoke-RestMethod -Uri "$HF/api/models/$repo`?blobs=true" -Headers $Headers -UseBasicParsing
    } catch {
        throw "Cannot read the file list of $repo from $HF (no network access, or a wrong name?): $($_.Exception.Message)"
    }
    # Top-level .json / .txt files, plus the two quantized ONNX files.
    $want = @($info.siblings | Where-Object {
        (($_.rfilename -notlike '*/*') -and ($_.rfilename -match '\.(json|txt)$')) -or ($WantOnnx -contains $_.rfilename)
    })
    foreach ($f in $WantOnnx) {
        if (-not ($want | Where-Object { $_.rfilename -eq $f })) {
            throw "$repo has no $f -- not a Transformers.js Whisper model?"
        }
    }
    foreach ($f in $want) {
        $path = $f.rfilename
        $size = $null
        if ($f.size) { $size = [int64]$f.size } elseif ($f.lfs -and $f.lfs.size) { $size = [int64]$f.lfs.size }
        $sha = $null
        if ($f.lfs -and $f.lfs.sha256) { $sha = $f.lfs.sha256 }
        $dest = Join-Path (Join-Path $Out $name) ($path -replace '/', [IO.Path]::DirectorySeparatorChar)
        $dir = Split-Path -Parent $dest
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

        if ((Test-Path $dest) -and $size -and ((Get-Item $dest).Length -eq $size)) {
            Write-Host ("  {0,-45} {1,12} bytes  already here" -f $path, $size)
        } else {
            Write-Host -NoNewline ("  {0,-45} {1,12} bytes  " -f $path, $(if ($size) { $size } else { '?' }))
            $tmp = "$dest.part"
            $tries = 0
            while ($true) {
                try {
                    Invoke-WebRequest -Uri "$HF/$repo/resolve/main/$path" -OutFile $tmp -Headers $Headers -UseBasicParsing
                    break
                } catch {
                    $tries++
                    if ($tries -ge 3) { throw "Download of $path failed: $($_.Exception.Message)" }
                    Start-Sleep -Seconds (2 * $tries)
                }
            }
            if ($size -and ((Get-Item $tmp).Length -ne $size)) {
                throw "$path size mismatch (got $((Get-Item $tmp).Length), expected $size)"
            }
            Move-Item -Force $tmp $dest
            Write-Host 'downloaded'
        }
        if ($sha) {
            $got = (Get-FileHash -Algorithm SHA256 -Path $dest).Hash.ToLower()
            if ($got -ne $sha.ToLower()) {
                Remove-Item -Force $dest
                throw "$path SHA-256 does not match Hugging Face's -- deleted, re-run to fetch again"
            }
            Write-Host ("  {0,-45} {1,12}        sha256 ok" -f '', '')
        }
        $Total += (Get-Item $dest).Length
    }
}

Write-Host ("== {0:N1} MB in {1}" -f ($Total / 1e6), $Out)
if (-not $NoZip) {
    $zip = "$Out.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path $Out -DestinationPath $zip
    Write-Host "== $zip"
}
