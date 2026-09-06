[CmdletBinding()]
param(
	[switch]$Fetch
)

$ErrorActionPreference = "Stop"

$expectedOriginPattern = "^(?:https://github\.com/|git@github\.com:|ssh://git@github\.com/)xup61069/loudness-correction-apo(?:\.git)?/?$"
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$solutionPath = Join-Path $repoRoot "HibikiEQAPO.sln"

if (!(Test-Path -LiteralPath $solutionPath -PathType Leaf)) {
	throw "HibikiEQAPO.sln was not found under $repoRoot"
}

$gitRootText = & git -C $repoRoot rev-parse --show-toplevel
if ($LASTEXITCODE -ne 0) {
	throw "Could not resolve the Git repository at $repoRoot"
}
$gitRoot = (Resolve-Path -LiteralPath $gitRootText.Trim()).Path
if (![string]::Equals($gitRoot, $repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
	throw "Git resolved to $gitRoot instead of $repoRoot"
}

$originUrl = (& git -C $repoRoot remote get-url origin).Trim()
if ($LASTEXITCODE -ne 0 -or $originUrl -notmatch $expectedOriginPattern) {
	throw "Unexpected origin remote: $originUrl"
}

if ($Fetch) {
	& git -C $repoRoot fetch origin --prune
	if ($LASTEXITCODE -ne 0) {
		throw "Could not refresh origin"
	}
}

$version = (& (Join-Path $PSScriptRoot "get-project-version.ps1")).Trim()
$branch = (& git -C $repoRoot branch --show-current).Trim()
if (!$branch) {
	$branch = "(detached HEAD)"
}
$head = (& git -C $repoRoot rev-parse --short=12 HEAD).Trim()

Write-Output "Repository: $repoRoot"
Write-Output "Origin: $originUrl"
Write-Output "Version: $version"
Write-Output "Branch: $branch"
Write-Output "HEAD: $head"

& git -C $repoRoot show-ref --verify --quiet refs/remotes/origin/main
if ($LASTEXITCODE -eq 0) {
	$originMain = (& git -C $repoRoot rev-parse --short=12 origin/main).Trim()
	$counts = ((& git -C $repoRoot rev-list --left-right --count origin/main...HEAD) -split "\s+")
	if ($LASTEXITCODE -ne 0 -or $counts.Count -ne 2) {
		throw "Could not compare HEAD with origin/main"
	}
	Write-Output "origin/main: $originMain"
	Write-Output "Ahead/behind: $($counts[1])/$($counts[0])"
} else {
	Write-Output "origin/main: unavailable (run with -Fetch)"
}

Write-Output ""
Write-Output "Working tree:"
& git -C $repoRoot status -sb --untracked-files=all
if ($LASTEXITCODE -ne 0) {
	throw "Could not read working-tree status"
}

Write-Output ""
Write-Output "Worktrees:"
& git -C $repoRoot worktree list
if ($LASTEXITCODE -ne 0) {
	throw "Could not list worktrees"
}
