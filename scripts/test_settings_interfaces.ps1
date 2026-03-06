param(
  [string]$BaseUrl = "http://192.168.0.138",
  [int]$TimeoutSec = 8
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) {
    throw "ASSERT FAILED: $Message"
  }
}

function Get-Json([string]$Url) {
  return Invoke-RestMethod -Method Get -Uri $Url -TimeoutSec $TimeoutSec
}

Write-Host "[TEST] Base URL: $BaseUrl"

# 1) Status reachable
$status = Get-Json "$BaseUrl/api/status"
Assert-True ($null -ne $status.display_mode) "api/status missing display_mode"
Write-Host "[OK] /api/status"

# 2) Set display mode
$modeResp = Get-Json "$BaseUrl/api/display?mode=1"
Assert-True ($modeResp.changed -eq $true) "api/display mode change not acknowledged"
Write-Host "[OK] /api/display?mode=1"

# 3) Read mode config
$cfg1 = Get-Json "$BaseUrl/api/mode/config?mode=1"
Assert-True ($cfg1.ok -eq $true) "api/mode/config read failed"
Write-Host "[OK] /api/mode/config read"

# 4) Volatile apply (persist=0)
$volatile = Get-Json "$BaseUrl/api/mode/config?set=1&persist=0&mode=1&hr=10&hg=20&hb=30&mr=40&mg=50&mb=60&sr=70&sg=80&sb=90&hw=5&mw=3&sw=7&sp=2"
Assert-True ($volatile.ok -eq $true) "volatile apply failed"
Assert-True ($volatile.persisted -eq $false) "volatile apply should not persist"
Write-Host "[OK] volatile apply"

$cfg2 = Get-Json "$BaseUrl/api/mode/config?mode=1"
Assert-True ($cfg2.hour.r -eq 10 -and $cfg2.hour.g -eq 20 -and $cfg2.hour.b -eq 30) "volatile hour color mismatch"
Assert-True ($cfg2.width.hour -eq 5 -and $cfg2.width.minute -eq 3 -and $cfg2.width.second -eq 7) "volatile width mismatch"
Write-Host "[OK] volatile readback"

# 5) Persistent apply (persist=1)
$persist = Get-Json "$BaseUrl/api/mode/config?set=1&persist=1&mode=1&hr=110&hg=120&hb=130&mr=140&mg=150&mb=160&sr=170&sg=180&sb=190&hw=9&mw=7&sw=11&sp=1"
Assert-True ($persist.ok -eq $true) "persistent apply failed"
Assert-True ($persist.persisted -eq $true) "persistent apply should persist"
Write-Host "[OK] persistent apply"

$cfg3 = Get-Json "$BaseUrl/api/mode/config?mode=1"
Assert-True ($cfg3.hour.r -eq 110 -and $cfg3.minute.g -eq 150 -and $cfg3.second.b -eq 190) "persistent color mismatch"
Assert-True ($cfg3.width.hour -eq 9 -and $cfg3.width.minute -eq 7 -and $cfg3.width.second -eq 11) "persistent width mismatch"
Write-Host "[OK] persistent readback"

# 6) Reset mode defaults
$reset = Get-Json "$BaseUrl/api/mode/config?reset=1&persist=1&mode=1"
Assert-True ($reset.ok -eq $true) "reset failed"
Assert-True ($reset.reset -eq $true) "reset flag missing"
Write-Host "[OK] reset defaults"

# 7) Brightness interface still reachable
$null = Invoke-RestMethod -Method Get -Uri "$BaseUrl/api/brightness?value=35" -TimeoutSec $TimeoutSec
Write-Host "[OK] /api/brightness"

Write-Host "[PASS] All settings interface checks passed"
