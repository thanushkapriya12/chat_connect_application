# Unified ChatConnect operations for 20-source-file edition.
# Includes original backup/restore implementation in one source file.
# Invoke with: powershell -NoProfile -File ops/operations.ps1 -Action backup
#              powershell -NoProfile -File ops/operations.ps1 -Action restore-test -BackupFolder "..."
[CmdletBinding()]
param(
    [ValidateSet('backup','restore-test','schedule','remove-schedule')]
    [string]$Action='backup',
    [string]$BackupFolder='',
    [string]$ProjectRoot='',
    [string]$BackupRoot=''
)
$ErrorActionPreference='Stop'

function Invoke-ChatConnectBackup {
function Assert-Exit([string]$Activity) {
    if ($LASTEXITCODE -ne 0) { throw "$Activity failed (exit code $LASTEXITCODE)." }
}
function Get-DockerInfo([string]$ContainerId) {
    $json = (& docker inspect $ContainerId) -join "`n"
    Assert-Exit 'Docker inspect'
    return @($json | ConvertFrom-Json)[0]
}
function Get-ContainerEnv([object]$Info, [string]$Key, [string]$Default) {
    $prefix = $Key + '='
    foreach ($item in @($Info.Config.Env)) {
        if ($item.StartsWith($prefix, [System.StringComparison]::Ordinal)) {
            return $item.Substring($prefix.Length)
        }
    }
    return $Default
}
function File-Record([string]$Path, [bool]$AllowEmpty = $false) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing backup file: $Path" }
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -le 0 -and -not $AllowEmpty) { throw "Backup file empty: $Path" }
    return @{ file = $file.Name; bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
}

if (-not $ProjectRoot) { $ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')) }
if (-not $BackupRoot) { $BackupRoot = Join-Path $env:USERPROFILE 'Documents\ChatConnectBackups' }
if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot 'compose.yaml') -PathType Leaf)) { throw 'ProjectRoot is not a ChatConnect Compose project.' }
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
New-Item -ItemType Directory -Path $BackupRoot -Force | Out-Null
$BackupRoot = (Resolve-Path -LiteralPath $BackupRoot).Path
# Never silently save personal data inside the project / future Git repository.
$rootNormalized = $ProjectRoot.TrimEnd([char[]]@('\','/'))
$backupNormalized = $BackupRoot.TrimEnd([char[]]@('\','/'))
if ($backupNormalized.StartsWith($rootNormalized + [IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -or $backupNormalized.Equals($rootNormalized,[System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'BackupRoot must be OUTSIDE the ChatConnect project to avoid accidental Git uploads.'
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$suffix = [Guid]::NewGuid().ToString('N').Substring(0,8)
$backupDir = Join-Path $BackupRoot "ChatConnect-$stamp-$suffix"
New-Item -ItemType Directory -Path $backupDir -ErrorAction Stop | Out-Null
Set-Content -LiteralPath (Join-Path $backupDir '.INCOMPLETE') -Value 'Backup not verified yet. Do not restore from this folder.' -Encoding Ascii
$tmpDbFile = "/tmp/chatconnect-backup-$suffix.dump"
$dbId = ''
try {
    Write-Host '=== ChatConnect Backup V1 ===' -ForegroundColor Cyan
    Write-Host "Backup folder: $backupDir"
    Write-Host 'Please keep conversations/uploads idle until this backup finishes.' -ForegroundColor Yellow
    Push-Location $ProjectRoot
    try {
        & docker compose config --quiet
        Assert-Exit 'Docker Compose config'
        $dbId = ((& docker compose ps -q db) -join '').Trim()
        Assert-Exit 'Get PostgreSQL container'
        $appId = ((& docker compose ps -q chatconnect) -join '').Trim()
        Assert-Exit 'Get ChatConnect container'
        if (-not $dbId -or -not $appId) { throw 'Both db and chatconnect containers must be running.' }
        $dbInfo = Get-DockerInfo $dbId
        $appInfo = Get-DockerInfo $appId
        if (-not $dbInfo.State.Running -or -not $appInfo.State.Running) { throw 'A ChatConnect container is not running.' }
        $dbUser = Get-ContainerEnv $dbInfo 'POSTGRES_USER' 'chatconnect'
        $dbName = Get-ContainerEnv $dbInfo 'POSTGRES_DB' 'chatconnect'
        $mediaMount = @($appInfo.Mounts | Where-Object { $_.Destination -eq '/app/uploads' -and $_.Type -eq 'volume' })
        if ($mediaMount.Count -ne 1) { throw 'Expected exactly one named Docker media volume mounted at /app/uploads.' }
        $mediaVolume = [string]$mediaMount[0].Name
        if ($mediaVolume -notmatch '^[a-zA-Z0-9][a-zA-Z0-9_.-]*$') { throw 'Unsafe Docker media volume name.' }
        Write-Host '[1/5] Exporting PostgreSQL custom-format dump...'
        & docker compose exec -T db pg_dump -U $dbUser -d $dbName --format=custom --no-owner --no-privileges "--file=$tmpDbFile"
        Assert-Exit 'pg_dump'
        & docker compose exec -T db pg_restore --list $tmpDbFile | Out-Null
        Assert-Exit 'pg_restore --list validation'
        & docker cp ("{0}:{1}" -f $dbId,$tmpDbFile) (Join-Path $backupDir 'database.dump')
        Assert-Exit 'docker cp PostgreSQL dump'

        Write-Host '[2/5] Archiving full media volume (photos, files, voice, pending uploads)...'
        # Uses existing postgres:16-alpine image. No network or writable access to original volume.
        $readOnlyMedia = "type=volume,source=$mediaVolume,target=/source,readonly"
        $writeBackup = "type=bind,source=$backupDir,target=/backup"
        $mediaCmd = 'cd /source && find . -type f -exec sha256sum {} \; > /backup/media_files.sha256 && tar -czf /backup/media.tar.gz -C /source .'
        & docker run --rm --network none --mount $readOnlyMedia --mount $writeBackup postgres:16-alpine sh -ec $mediaCmd
        Assert-Exit 'Media backup'
        & docker run --rm --network none --mount "type=bind,source=$backupDir,target=/backup,readonly" postgres:16-alpine sh -ec 'tar -tzf /backup/media.tar.gz > /dev/null'
        Assert-Exit 'Media gzip archive integrity check'

        Write-Host '[3/5] Saving safe project source snapshot (NO .env/secrets)...'
        $stage = Join-Path $backupDir '_source_staging'
        New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
        $safeExt = @('.cpp','.c','.hpp','.h','.md','.txt','.ps1','.bat','.json','.yaml','.yml')
        foreach ($file in @(Get-ChildItem -LiteralPath $ProjectRoot -File)) {
            if ($file.Name -match '^\.env' -or $file.Name -match '\.(dump|db|sqlite|zip|tar|gz|log)$') { continue }
            if ($safeExt -contains $file.Extension.ToLowerInvariant() -or @('Dockerfile','Makefile','CMakeLists.txt','.dockerignore','.gitignore') -contains $file.Name) {
                Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $stage $file.Name) -ErrorAction Stop
            }
        }
        # Copy only static app/source asset extensions. Never accidentally snapshot nested .env/keys.
        $safeAssetExt = @('.js','.css','.html','.htm','.webp','.jpeg','.jpg','.png','.svg','.gif','.ico','.avif','.woff','.woff2','.md','.txt','.json','.cpp','.c','.hpp','.h','.ps1','.bat','.yaml','.yml')
        foreach ($tree in @('web','tests')) {
            $treeRoot = Join-Path $ProjectRoot $tree
            if (-not (Test-Path -LiteralPath $treeRoot -PathType Container)) { continue }
            foreach ($asset in @(Get-ChildItem -LiteralPath $treeRoot -Recurse -File)) {
                if ($asset.Name -match '^\.env' -or $asset.Name -match '(?i)(secret|private.?key|password)' -or ($safeAssetExt -notcontains $asset.Extension.ToLowerInvariant())) { continue }
                $relative = $asset.FullName.Substring($treeRoot.Length).TrimStart([char[]]@('\','/'))
                $destination = Join-Path (Join-Path $stage $tree) $relative
                $destParent = Split-Path -Parent $destination
                if (-not (Test-Path -LiteralPath $destParent)) { New-Item -ItemType Directory -Path $destParent -Force | Out-Null }
                Copy-Item -LiteralPath $asset.FullName -Destination $destination -ErrorAction Stop
            }
        }
        if (Test-Path -LiteralPath (Join-Path $ProjectRoot '.env.example')) { Copy-Item -LiteralPath (Join-Path $ProjectRoot '.env.example') -Destination (Join-Path $stage '.env.example') }
        if (Test-Path -LiteralPath (Join-Path $stage '.env')) { throw 'Secret .env unexpectedly in snapshot staging.' }
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath (Join-Path $backupDir 'source_snapshot.zip') -CompressionLevel Optimal -Force
        Remove-Item -LiteralPath $stage -Recurse -Force

        Write-Host '[4/5] Calculating SHA-256 checksums...'
        $manifest = [ordered]@{
            formatVersion = 1
            createdUtc = (Get-Date).ToUniversalTime().ToString('o')
            backupMethod = 'pg_dump custom format + read-only media volume tar + safe source snapshot'
            note = 'Contains sensitive private messages and user-uploaded content. Keep private. Restore test separate.'
            database = (File-Record (Join-Path $backupDir 'database.dump'))
            media = (File-Record (Join-Path $backupDir 'media.tar.gz'))
            mediaFileHashes = (File-Record (Join-Path $backupDir 'media_files.sha256') $true)
            source = (File-Record (Join-Path $backupDir 'source_snapshot.zip'))
        }
        $json = $manifest | ConvertTo-Json -Depth 6
        [IO.File]::WriteAllText((Join-Path $backupDir 'manifest.json'), $json, (New-Object System.Text.UTF8Encoding($false)))
        Remove-Item -LiteralPath (Join-Path $backupDir '.INCOMPLETE') -Force
        Write-Host '[5/5] Backup created and archives/checksums validated.' -ForegroundColor Green
        Write-Host "SUCCESS BACKUP: $backupDir" -ForegroundColor Green
        Write-Host 'NEXT: Run RUN_RESTORE_TEST.bat with this folder to verify isolated recovery.' -ForegroundColor Yellow
    }
    finally { Pop-Location }
}
catch {
    Write-Host "BACKUP FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Incomplete folder (do not use for restore): $backupDir" -ForegroundColor Yellow
    exit 1
}
finally {
    if ($dbId) { & docker exec $dbId rm -f $tmpDbFile 2>$null | Out-Null }
}

}

function Invoke-ChatConnectRestore {
function Assert-Exit([string]$Activity) {
    if ($LASTEXITCODE -ne 0) { throw "$Activity failed (exit code $LASTEXITCODE)." }
}
function Validate-File([object]$Record) {
    $path = Join-Path $BackupFolder $Record.file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Backup missing: $($Record.file)" }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Record.sha256.ToLowerInvariant()) { throw "SHA-256 mismatch: $($Record.file)" }
    if ((Get-Item -LiteralPath $path).Length -ne [long]$Record.bytes) { throw "Size mismatch: $($Record.file)" }
    Write-Host "[PASS] Checksum: $($Record.file)"
}
$testId = (Get-Date -Format 'yyyyMMddHHmmss') + ([Guid]::NewGuid().ToString('N').Substring(0,6))
$container = "cc-restore-test-$testId"
$dbVol = "cc_restore_db_$testId"
$mediaVol = "cc_restore_media_$testId"
$hasDbVolume=$false
$hasMediaVolume=$false
$started=$false
$testFailed = $false
try {
    $BackupFolder = (Resolve-Path -LiteralPath $BackupFolder -ErrorAction Stop).Path
    if (Test-Path -LiteralPath (Join-Path $BackupFolder '.INCOMPLETE')) { throw 'Backup incomplete. Refusing restore test.' }
    $manifestPath = Join-Path $BackupFolder 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'manifest.json missing.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.formatVersion -ne 1) { throw 'Unsupported manifest version.' }
    Write-Host '=== ChatConnect RESTORE TEST - ISOLATED ONLY ===' -ForegroundColor Cyan
    Write-Host 'No changes will be made to existing ChatConnect DB, media or containers.' -ForegroundColor Cyan
    foreach ($record in @($manifest.database, $manifest.media, $manifest.mediaFileHashes, $manifest.source)) { Validate-File $record }

    Write-Host '[1/4] Preparing isolated disposable PostgreSQL test container...'
    & docker volume create $dbVol | Out-Null
    Assert-Exit 'Create temp PostgreSQL volume'
    $hasDbVolume=$true
    & docker volume create $mediaVol | Out-Null
    Assert-Exit 'Create temp media volume'
    $hasMediaVolume=$true
    # No exposed ports, no Docker network; credentials are throwaway test-only values.
    $testDbUser='ccrestore'
    $testDbName='chatconnect_restore'
    & docker run -d --network none --name $container --mount "type=volume,source=$dbVol,target=/var/lib/postgresql/data" -e POSTGRES_USER=$testDbUser -e POSTGRES_PASSWORD=restore-test-temporary -e POSTGRES_DB=$testDbName postgres:16-alpine | Out-Null
    Assert-Exit 'Start isolated PostgreSQL container'
    $started=$true
    $ready=$false
    for($i=0; $i -lt 45; $i++) {
        & docker exec $container pg_isready -U $testDbUser -d $testDbName > $null 2>&1
        if ($LASTEXITCODE -eq 0) { $ready=$true; break }
        Start-Sleep -Seconds 1
    }
    if (-not $ready) { throw 'Isolated PostgreSQL test container failed to start.' }

    Write-Host '[2/4] Restoring database.dump into temporary database...'
    & docker cp (Join-Path $BackupFolder 'database.dump') ("{0}:/tmp/database.dump" -f $container)
    Assert-Exit 'Copy dump to restore-test container'
    & docker exec $container pg_restore --exit-on-error --no-owner --no-privileges -U $testDbUser -d $testDbName /tmp/database.dump
    Assert-Exit 'pg_restore into isolated PostgreSQL'
    $tableCountText = ((& docker exec $container psql -U $testDbUser -d $testDbName -tA -c "SELECT count(*) FROM information_schema.tables WHERE table_schema='public' AND table_type='BASE TABLE';") -join '').Trim()
    Assert-Exit 'Query restored tables'
    $tableCount=0
    if (-not [int]::TryParse($tableCountText, [ref]$tableCount) -or $tableCount -lt 1) { throw "Database restored but no user tables found ($tableCountText)." }
    Write-Host "[PASS] Restored $tableCount public tables." -ForegroundColor Green

    Write-Host '[3/4] Restoring media into a separate temporary Docker volume...'
    $mediaBind = "type=bind,source=$BackupFolder,target=/backup,readonly"
    $tempMediaMount = "type=volume,source=$mediaVol,target=/restore"
    $cmd = 'tar -xzf /backup/media.tar.gz -C /restore && cd /restore && if [ -s /backup/media_files.sha256 ]; then sha256sum -c /backup/media_files.sha256 > /dev/null; fi'
    & docker run --rm --network none --mount $tempMediaMount --mount $mediaBind postgres:16-alpine sh -ec $cmd
    Assert-Exit 'Restore + SHA-256 verify media files'
    Write-Host '[PASS] Every restored media file matches original SHA-256 hashes.' -ForegroundColor Green
    Write-Host '[4/4] Isolated restore test succeeded.' -ForegroundColor Green
    Write-Host 'SUCCESS: DATABASE + MEDIA RESTORED AND VERIFIED (isolated containers/volumes only).' -ForegroundColor Green
}
catch {
    Write-Host "RESTORE TEST FAILED: $($_.Exception.Message)" -ForegroundColor Red
    $testFailed = $true
}
finally {
    Write-Host 'Cleaning up ONLY disposable restore-test resources...'
    if ($started) { & docker rm -f $container > $null 2>&1 }
    if ($hasDbVolume) { & docker volume rm $dbVol > $null 2>&1 }
    if ($hasMediaVolume) { & docker volume rm $mediaVol > $null 2>&1 }
    Write-Host 'Live ChatConnect volumes were not touched.'
}

if ($testFailed) { exit 1 }

}

switch ($Action) {
    'backup' { Invoke-ChatConnectBackup }
    'restore-test' {
        if(-not $BackupFolder) {throw 'Required: -BackupFolder for restore-test'}
        Invoke-ChatConnectRestore
    }
    'schedule' {
        Import-Module ScheduledTasks -ErrorAction Stop
        $scriptFile=$PSCommandPath
        $args='-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+$scriptFile+'" -Action backup'
        $actionObj=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $args -WorkingDirectory (Split-Path $PSScriptRoot -Parent)
        $trigger=New-ScheduledTaskTrigger -Daily -At '21:00'
        $user=[System.Security.Principal.WindowsIdentity]::GetCurrent().Name
        $principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel LeastPrivilege
        Register-ScheduledTask -TaskName 'ChatConnectDailyLocalBackup' -Action $actionObj -Trigger $trigger -Principal $principal -Force
        Write-Host 'Daily local backup scheduled.'
    }
    'remove-schedule' {
        Unregister-ScheduledTask -TaskName 'ChatConnectDailyLocalBackup' -Confirm:$false -ErrorAction Stop
    }
}
