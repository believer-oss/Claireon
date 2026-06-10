#Requires -Version 5.1

<#
.SYNOPSIS
    Returns the git username extracted from the user's email address.

.DESCRIPTION
    Gets the git user.email config value and extracts the username portion
    (everything before the @ symbol). This provides a consistent identifier
    for branch naming and other automation purposes.

.OUTPUTS
    String. The username portion of the git email (e.g., "jdoe" from "jdoe@example.com")

.EXAMPLE
    .\Get-GitUsername.ps1
    jdoe

.EXAMPLE
    $username = & .\Get-GitUsername.ps1
#>

$ErrorActionPreference = "Stop"

# Get git user email
$email = git config user.email 2>$null

if ([string]::IsNullOrWhiteSpace($email)) {
    Write-Error "Git user.email is not configured. Run: git config user.email <your-email>"
    exit 1
}

# Extract username (part before @)
if ($email -match "^([^@]+)@") {
    Write-Output $Matches[1]
    exit 0
} else {
    Write-Error "Invalid email format: $email"
    exit 1
}
