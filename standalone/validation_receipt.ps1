Set-StrictMode -Version Latest
function Assert-TaxiNativeReceipt([string]$Directory) {
    $receiptPath = Join-Path $Directory 'validation.json'
    $receipt = Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json
    if (-not $receipt.passed -or $receipt.version -ne '0.8.0') { throw 'A successful native validation receipt is required.' }
    foreach ($name in @('380-taxi-cam.exe','taxi-camera-bridge.dll')) {
        $file = Join-Path $Directory $name
        $expected = $receipt.files.PSObject.Properties[$name].Value
        if (-not $expected -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected) { throw "Native binary differs from the validated build: $name" }
    }
    return $receipt
}
