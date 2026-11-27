param(
    [string]$UnrealEditor = 'M:/Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe',
    [string]$ProfileRoot = '',
    [switch]$IncludeOptions
)

# All profiles are isolated from the player's normal saves/settings. Audio stays muted.
$ErrorActionPreference = 'Stop'
$reviewProjectRoot = Split-Path -Parent $PSScriptRoot
$reviewProject = Join-Path $reviewProjectRoot 'UnrealAA.uproject'
if (-not $ProfileRoot) {
    $ProfileRoot = Join-Path $reviewProjectRoot ('Saved/ItemOwnerReview-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
}
if (Test-Path -LiteralPath $ProfileRoot) {
    throw "Use a fresh profile directory so previous settings cannot mask failures: $ProfileRoot"
}
if (-not (Test-Path -LiteralPath $UnrealEditor -PathType Leaf)) { throw "Editor executable missing: $UnrealEditor" }

$reviewCases = @(
    @{ Mode = 'ItemOwners'; Map = '/Game/Maps/L30'; Profile = 'Owners'; Render = $false },
    @{ Mode = 'ItemOwnersReload'; Map = '/Game/Maps/L301?WadLoad=NativeWadItemOwnersReview'; Profile = 'Owners'; Render = $false },
    @{ Mode = 'ItemOwnersCompleteReload'; Map = '/Game/Maps/L301?WadLoad=NativeWadItemOwnersCompleteReview'; Profile = 'Owners'; Render = $false }
)
if ($IncludeOptions) {
    $reviewCases += @(
        @{ Mode = 'Options'; Map = '/Game/Maps/L30'; Profile = 'Options'; Render = $true },
        @{ Mode = 'OptionsReload'; Map = '/Game/Maps/L30'; Profile = 'Options'; Render = $true }
    )
}

foreach ($reviewCase in $reviewCases) {
    $reviewName = 'PlayableLoops-ItemOwners-' + $reviewCase.Mode
    $reviewLog = Join-Path $reviewProjectRoot ('Saved/Logs/' + $reviewName + '.log')
    $reviewImage = Join-Path $reviewProjectRoot ('Saved/' + $reviewName + '.png')
    $reviewUser = Join-Path $ProfileRoot $reviewCase.Profile
    $reviewRenderer = if ($reviewCase.Render) { '-RenderOffscreen' } else { '-nullrhi' }
    & $UnrealEditor $reviewProject $reviewCase.Map -game -unattended -nop4 -nosplash -NoEOS -nosound $reviewRenderer -stdout -DDC-ForceMemoryCache "-UserDir=$reviewUser" -UseFixedTimeStep -FPS=60 -ResX=640 -ResY=400 '-TestExit=Native WAD port review finished' "-abslog=$reviewLog" "-ExecCmds=AA.ExportNativeWadPortReview $($reviewCase.Mode) $reviewImage 720" *> ($reviewLog + '.stdout')
    $reviewExitCode = $LASTEXITCODE
    $reviewText = Get-Content -LiteralPath $reviewLog -Raw
    $reviewText -split "\r?\n" | Select-String 'Verified |review failed|review finished'
    # Unreal can exit zero after a review failure; require the actual result as well.
    if ($reviewExitCode -ne 0 -or $reviewText -match 'review failed' -or $reviewText -notmatch 'Native WAD port review finished') {
        throw "Failed $($reviewCase.Mode); inspect $reviewLog"
    }
}
Write-Output "Verified native item owners/knowledge in separate processes. Isolated profile: $ProfileRoot"
