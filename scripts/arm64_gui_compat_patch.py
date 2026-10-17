#!/usr/bin/env python3
"""Build-time ARM64-only hardening for the pinned Tauri Control Panel gitlink.

The upstream Panel only publishes windows-x64-installer update assets.  When
the native ARM64 GUI experiment is running, never advertise/download/execute
x64 installers or attempt an incompatible in-place core replacement.

This intentionally modifies only a disposable CI checkout of the pinned
submodule, NOT the upstream repository and NOT the Sunshine source code.
"""
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("Usage: python scripts/arm64_gui_compat_patch.py <gui-submodule-root>")

root = Path(sys.argv[1]).resolve()
path = root / "src-tauri" / "src" / "update.rs"
if not path.is_file():
    raise SystemExit(f"Required GUI source missing: {path}")

data = path.read_text(encoding="utf-8")

def replace_once(old: str, new: str, label: str):
    global data
    n = data.count(old)
    if n != 1:
        raise RuntimeError(f"{label} unexpected GUI update.rs version: expected 1 anchor, found {n}")
    data = data.replace(old, new, 1)

replace_once(
    '''fn preferred_asset_type() -> &'static str {
    // The Panel only supports the Windows Installer update flow. Portable
    // archives may remain in metadata for other consumers, but are never
    // selected or downloaded here.
    "windows-x64-installer"
}''',
    '''fn preferred_asset_type() -> &'static str {
    // Experimental native ARM64 portable Sunshine has no compatible signed
    // installer yet. Never advertise x64 installers in its update UI.
    #[cfg(target_arch = "aarch64")]
    { "windows-arm64-installer-unavailable" }
    #[cfg(not(target_arch = "aarch64"))]
    { "windows-x64-installer" }
}''',
    "asset selection architecture guard"
)

replace_once(
    '''fn run_installer_and_wait(state: &UpdaterHelperState) -> Result<i32, String> {
    use std::os::windows::process::CommandExt;''',
    '''fn run_installer_and_wait(state: &UpdaterHelperState) -> Result<i32, String> {
    #[cfg(target_arch = "aarch64")]
    {
        let _ = state;
        return Err("ARM64 portable experiment: installer auto-update is disabled until native signed packages exist".to_string());
    }
    use std::os::windows::process::CommandExt;''',
    "installer execution architecture guard"
)

replace_once(
    '''pub async fn download_update(
    url: String,
    fallback_url: Option<String>,
    filename: String,
    expected_size: Option<u64>,
    expected_sha256: Option<String>,
    asset_type: Option<String>,
    app_handle: AppHandle,
) -> Result<serde_json::Value, DownloadCommandError> {
    validate_download_url(&url, Some(&filename))''',
    '''pub async fn download_update(
    url: String,
    fallback_url: Option<String>,
    filename: String,
    expected_size: Option<u64>,
    expected_sha256: Option<String>,
    asset_type: Option<String>,
    app_handle: AppHandle,
) -> Result<serde_json::Value, DownloadCommandError> {
    #[cfg(target_arch = "aarch64")]
    {
        let _ = (&url, &fallback_url, &filename, &expected_size, &expected_sha256, &asset_type, &app_handle);
        return Err(DownloadCommandError::new(
            DownloadErrorCode::FilePreparationFailed,
            "ARM64 portable experiment: installer updates are not supported".to_string(),
        ));
    }
    #[cfg(not(target_arch = "aarch64"))]
    validate_download_url(&url, Some(&filename))''',
    "installer download architecture guard"
)

# Rust's cfg on the first statement alone would remove the first guard on
# other targets, leaving the remaining original implementation untouched.
# On ARM64, returning early must not turn subsequent variables into moved
# values: these source files are compiled only in the ARM64 target workflow.
path.write_text(data, encoding="utf-8")
print("Applied ARM64-only GUI updater safety guards to pinned Control Panel checkout")
