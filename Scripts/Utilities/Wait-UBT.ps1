param([int]$PollSeconds = 60)
do {
    $ubt = Get-Process -Name dotnet -ErrorAction SilentlyContinue
    if ($ubt) {
        Write-Host "$(Get-Date -Format 'HH:mm:ss') - UBT still running (PIDs: $($ubt.Id -join ', ')), waiting ${PollSeconds}s..."
        Start-Sleep -Seconds $PollSeconds
    }
} while ($ubt)
Write-Host "$(Get-Date -Format 'HH:mm:ss') - UBT process finished!"
