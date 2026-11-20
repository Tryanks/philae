//! Code signing via the system `codesign`/`security` tools.

use anyhow::{bail, Context, Result};
use plist::{Dictionary, Value};
use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

pub struct Profile {
    pub path: PathBuf,
    pub name: String,
    pub team_id: String,
    /// App id pattern without the team prefix, e.g. `com.example.app` or `*`.
    pub app_id: String,
    pub expires: SystemTime,
    entitlements: Dictionary,
}

impl Profile {
    pub fn load(path: &Path) -> Result<Self> {
        let out = Command::new("security")
            .args(["cms", "-D", "-i"])
            .arg(path)
            .output()
            .context("running `security cms`")?;
        if !out.status.success() {
            bail!("{} is not a readable provisioning profile", path.display());
        }
        let root = Value::from_reader(Cursor::new(out.stdout))?
            .into_dictionary()
            .context("profile is not a plist dictionary")?;
        let entitlements = root
            .get("Entitlements")
            .and_then(Value::as_dictionary)
            .context("profile has no Entitlements")?
            .clone();
        let full_app_id = entitlements
            .get("application-identifier")
            .and_then(Value::as_string)
            .context("profile has no application-identifier")?;
        let (team_id, app_id) = full_app_id
            .split_once('.')
            .context("malformed application-identifier")?;
        Ok(Self {
            path: path.to_owned(),
            name: root
                .get("Name")
                .and_then(Value::as_string)
                .unwrap_or("?")
                .to_owned(),
            team_id: team_id.to_owned(),
            app_id: app_id.to_owned(),
            expires: root
                .get("ExpirationDate")
                .and_then(Value::as_date)
                .map(SystemTime::from)
                .context("profile has no ExpirationDate")?,
            entitlements,
        })
    }

    pub fn matches(&self, bundle_id: &str) -> bool {
        match self.app_id.strip_suffix('*') {
            Some(prefix) => bundle_id.starts_with(prefix),
            None => self.app_id == bundle_id,
        }
    }

    pub fn expired(&self) -> bool {
        self.expires <= SystemTime::now()
    }
}

/// Unexpired profiles installed by Xcode for this user.
pub fn installed_profiles() -> Vec<Profile> {
    let Some(home) = std::env::var_os("HOME").map(PathBuf::from) else {
        return Vec::new();
    };
    [
        "Library/Developer/Xcode/UserData/Provisioning Profiles",
        "Library/MobileDevice/Provisioning Profiles",
    ]
    .iter()
    .filter_map(|dir| fs::read_dir(home.join(dir)).ok())
    .flatten()
    .filter_map(|entry| Profile::load(&entry.ok()?.path()).ok())
    .filter(|p| !p.expired())
    .collect()
}

/// SHA-1 of the first valid code signing identity in the keychain.
fn default_identity() -> Result<String> {
    let out = Command::new("security")
        .args(["find-identity", "-v", "-p", "codesigning"])
        .output()
        .context("running `security find-identity`")?;
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|line| line.split_whitespace().nth(1))
        .find(|hash| hash.len() == 40 && hash.bytes().all(|b| b.is_ascii_hexdigit()))
        .map(str::to_owned)
        .context("no code signing identity in the keychain (sign in to Xcode to create one)")
}

pub fn sign_adhoc(app: &Path) -> Result<()> {
    sign_nested_code(app, "-")?;
    codesign(app, "-", None)
}

/// Sign the dylibs inside the bundle. Nested code must be signed before the bundle that
/// contains it, because the bundle's signature covers theirs.
fn sign_nested_code(app: &Path, identity: &str) -> Result<()> {
    let Ok(entries) = fs::read_dir(app.join("Frameworks")) else {
        return Ok(());
    };
    for entry in entries {
        let path = entry?.path();
        if path.extension().is_some_and(|e| e == "dylib") {
            codesign(&path, identity, None)?;
        }
    }
    Ok(())
}

pub fn sign_for_device(
    app: &Path,
    bundle_id: &str,
    identity: Option<&str>,
    profile: &Profile,
) -> Result<()> {
    if profile.expired() {
        bail!("provisioning profile “{}” has expired", profile.name);
    }
    if !profile.matches(bundle_id) {
        bail!(
            "provisioning profile “{}” covers {}, not {bundle_id}",
            profile.name,
            profile.app_id
        );
    }
    fs::copy(&profile.path, app.join("embedded.mobileprovision")).context("embedding profile")?;

    // The signature must carry the profile's entitlements with the wildcard made concrete.
    let mut entitlements = profile.entitlements.clone();
    entitlements.insert(
        "application-identifier".into(),
        format!("{}.{bundle_id}", profile.team_id).into(),
    );
    let ent_path = app.with_extension("entitlements.plist");
    Value::Dictionary(entitlements).to_file_xml(&ent_path)?;

    let identity = match identity {
        Some(i) => i.to_owned(),
        None => default_identity()?,
    };
    let result =
        sign_nested_code(app, &identity).and_then(|()| codesign(app, &identity, Some(&ent_path)));
    let _ = fs::remove_file(&ent_path);
    result
}

fn codesign(code: &Path, identity: &str, entitlements: Option<&Path>) -> Result<()> {
    let mut cmd = Command::new("codesign");
    cmd.args([
        "--force",
        "--timestamp=none",
        "--generate-entitlement-der",
        "--sign",
        identity,
    ]);
    if let Some(e) = entitlements {
        cmd.arg("--entitlements").arg(e);
    }
    let out = cmd.arg(code).output().context("running codesign")?;
    if !out.status.success() {
        bail!(
            "codesign failed: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    Ok(())
}
