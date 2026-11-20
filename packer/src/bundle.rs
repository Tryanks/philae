//! Assemble the iOS `.app` bundle: runtime host binary + Info.plist + icons + Android payload.

use crate::apk::Apk;
use crate::elf2macho;
use crate::icon::{self, IconSource};
use anyhow::{bail, Context, Result};
use plist::{Dictionary, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const HOST_EXECUTABLE: &str = "A2IHost";
// The runtime needs os_sync_wait_on_address (its futex), introduced in iOS 17.4.
const MIN_IOS: &str = "17.4";

#[derive(Clone, Copy, PartialEq, Eq, Debug, clap::ValueEnum)]
pub enum Target {
    /// iOS Simulator (ad-hoc signed)
    Sim,
    /// Physical iPhone/iPad (needs a signing identity and provisioning profile)
    Device,
}

impl Target {
    /// Xcode SDK name; also the subdirectory of the runtime dir holding the host binary.
    pub fn sdk(self) -> &'static str {
        match self {
            Target::Sim => "iphonesimulator",
            Target::Device => "iphoneos",
        }
    }

    pub fn platform(self) -> elf2macho::Platform {
        match self {
            Target::Sim => elf2macho::Platform::IosSimulator,
            Target::Device => elf2macho::Platform::Ios,
        }
    }
}

/// The runtime library that provides Android's C library to the app's native code.
pub const BIONIC_INSTALL_NAME: &str = "@rpath/liba2i_bionic.dylib";
const BIONIC_FILE: &str = "liba2i_bionic.dylib";

/// The Android functions the bionic library provides: its exports named `bionic_<name>`.
pub fn bionic_exports(library: &Path) -> Result<BTreeSet<String>> {
    let out = Command::new("nm")
        .args(["-gU"])
        .arg(library)
        .output()
        .context("running nm")?;
    if !out.status.success() {
        bail!("cannot read the exports of {}", library.display());
    }
    Ok(String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|line| {
            line.rsplit(' ')
                .next()?
                .strip_prefix("_bionic_")
                .map(str::to_owned)
        })
        .collect())
}

/// Converts the app's native libraries (`lib/arm64-v8a/*.so`) into dylibs next to the
/// runtime's. Returns what could not be converted or will not work, as warnings.
fn convert_native_libraries(
    apk: &mut Apk,
    names: &[String],
    frameworks: &Path,
    target: Target,
) -> Result<Vec<String>> {
    let mut warnings = Vec::new();
    let bionic = bionic_exports(&frameworks.join(BIONIC_FILE))?;
    let mut libraries = BTreeMap::new();
    for name in names {
        libraries.insert(name.clone(), apk.read(&format!("lib/arm64-v8a/{name}"))?);
    }
    let mut exports = BTreeMap::new();
    for (name, data) in &libraries {
        match elf2macho::exported_symbols(data) {
            Ok(symbols) => {
                exports.insert(name.clone(), symbols);
            }
            Err(e) => warnings.push(format!("native library {name} was not converted: {e:#}")),
        }
    }
    for (name, data) in &libraries {
        if !exports.contains_key(name) {
            continue;
        }
        let output = frameworks.join(elf2macho::dylib_name(name));
        if output.exists() {
            warnings.push(format!(
                "native library {name} was not converted: the runtime has a library of that name"
            ));
            continue;
        }
        let options = elf2macho::Options {
            install_name: &elf2macho::sibling_install_name(name),
            platform: target.platform(),
            bionic: BIONIC_INSTALL_NAME,
            bionic_exports: Some(&bionic),
            siblings: &exports,
        };
        match elf2macho::convert(data, &options) {
            Ok(converted) => {
                fs::write(&output, &converted.macho)
                    .with_context(|| format!("writing {}", output.display()))?;
                if !converted.missing.is_empty() {
                    warnings.push(format!(
                        "native library {name} uses {} C library function(s) the runtime lacks, and will abort if it calls one: {}",
                        converted.missing.len(),
                        converted.missing.join(" ")
                    ));
                }
            }
            Err(e) => warnings.push(format!("native library {name} was not converted: {e:#}")),
        }
    }
    Ok(warnings)
}

pub struct Built {
    pub label: String,
    pub icon: IconSource,
    pub dex: Vec<String>,
    pub warnings: Vec<String>,
}

pub fn build(
    apk: &mut Apk,
    apk_path: &Path,
    app_dir: &Path,
    target: Target,
    bundle_id: &str,
    runtime_dir: &Path,
) -> Result<Built> {
    let host = runtime_dir.join(target.sdk()).join(HOST_EXECUTABLE);
    if !host.is_file() {
        bail!(
            "runtime host not found at {} — build it first with scripts/build-runtime.sh",
            host.display()
        );
    }
    let payload = app_dir.join("android");
    fs::create_dir_all(&payload)?;

    let exe = app_dir.join(HOST_EXECUTABLE);
    fs::copy(&host, &exe).context("copying runtime host")?;
    fs::set_permissions(&exe, fs::Permissions::from_mode(0o755))?;

    // The Android runtime: its libraries (ART, ICU, libcore natives) and the files it boots
    // from (boot class path, ICU and time zone data), which are the same for every target.
    let frameworks = runtime_dir.join(target.sdk()).join("Frameworks");
    let android_root = runtime_dir.join("android-root");
    if !frameworks.is_dir() || !android_root.join("apex").is_dir() {
        bail!(
            "runtime is incomplete under {} — run scripts/fetch-android-image.sh, \
             scripts/build-java.sh and scripts/build-runtime.sh",
            runtime_dir.display()
        );
    }
    copy_tree(&frameworks, &app_dir.join("Frameworks")).context("copying runtime libraries")?;
    copy_tree(&android_root, &app_dir.join("android-root")).context("copying Android root")?;

    // The APK goes into the bundle untouched: ART loads classes and the Android framework
    // reads resources straight out of it, and apps that inspect their own package expect it intact.
    fs::copy(apk_path, payload.join("base.apk")).context("copying APK into bundle")?;
    let dex = apk.dex_names();

    let label = apk.label();
    let mut warnings = Vec::new();
    let native_libs = apk.native_libs();
    if !native_libs.is_empty() && !native_libs.contains_key("arm64-v8a") {
        warnings.push(format!(
            "native libraries exist only for {:?}; iOS devices need arm64-v8a",
            native_libs.keys().collect::<Vec<_>>()
        ));
    }
    if let Some(names) = native_libs.get("arm64-v8a") {
        warnings.extend(convert_native_libraries(
            apk,
            names,
            &app_dir.join("Frameworks"),
            target,
        )?);
    }
    if dex.is_empty() {
        warnings.push("APK contains no classes.dex".to_owned());
    }
    if apk.manifest.launcher_activity.is_none() {
        warnings.push("no launcher activity declared; nothing to start".to_owned());
    }

    let m = &apk.manifest;
    let descriptor = serde_json::json!({
        "package": m.package,
        "label": label,
        "versionName": m.version_name,
        "versionCode": m.version_code,
        "minSdk": m.min_sdk,
        "targetSdk": m.target_sdk,
        "application": m.application_class,
        "launcherActivity": m.launcher_activity,
        "components": {
            "activities": m.activities.len(),
            "services": m.services,
            "receivers": m.receivers,
            "providers": m.providers,
        },
        "permissions": m.permissions,
        "dex": dex,
        "nativeLibs": native_libs,
        "packer": env!("CARGO_PKG_VERSION"),
    });
    fs::write(
        payload.join("a2i.json"),
        serde_json::to_vec_pretty(&descriptor)?,
    )?;

    let (master, icon_source) = icon::render(apk);
    // (file name, pixel size); names follow the CFBundleIconFiles "<base><scale><idiom>" scheme.
    for (file, size) in [
        ("AppIcon60x60@2x.png", 120),
        ("AppIcon60x60@3x.png", 180),
        ("AppIcon76x76@2x~ipad.png", 152),
        ("AppIcon83.5x83.5@2x~ipad.png", 167),
    ] {
        icon::scaled(&master, size)
            .save(app_dir.join(file))
            .with_context(|| format!("writing {file}"))?;
    }

    info_plist(apk, &label, target, bundle_id)
        .to_file_xml(app_dir.join("Info.plist"))
        .context("writing Info.plist")?;
    fs::write(app_dir.join("PkgInfo"), b"APPL????")?;

    Ok(Built {
        label,
        icon: icon_source,
        dex,
        warnings,
    })
}

fn copy_tree(from: &Path, to: &Path) -> Result<()> {
    fs::create_dir_all(to)?;
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        // Follow symlinks: the bundle must be self-contained.
        if fs::metadata(entry.path())?.is_dir() {
            copy_tree(&entry.path(), &target)?;
        } else {
            fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

fn info_plist(apk: &Apk, label: &str, target: Target, bundle_id: &str) -> Value {
    let m = &apk.manifest;
    let strings = |items: &[&str]| Value::Array(items.iter().map(|s| Value::from(*s)).collect());
    let mut d = Dictionary::new();
    let mut set = |k: &str, v: Value| {
        d.insert(k.to_owned(), v);
    };

    set("CFBundleExecutable", HOST_EXECUTABLE.into());
    set("CFBundleIdentifier", bundle_id.into());
    set("CFBundleName", label.into());
    set("CFBundleDisplayName", label.into());
    set("CFBundlePackageType", "APPL".into());
    set("CFBundleInfoDictionaryVersion", "6.0".into());
    set(
        "CFBundleShortVersionString",
        marketing_version(m.version_name.as_deref()).into(),
    );
    set("CFBundleVersion", m.version_code.max(1).to_string().into());
    set("MinimumOSVersion", MIN_IOS.into());
    set("LSRequiresIPhoneOS", true.into());
    set("DTPlatformName", target.sdk().into());
    set(
        "CFBundleSupportedPlatforms",
        strings(&[if target == Target::Sim {
            "iPhoneSimulator"
        } else {
            "iPhoneOS"
        }]),
    );
    set("UIDeviceFamily", Value::Array(vec![1.into(), 2.into()]));
    set("UIRequiredDeviceCapabilities", strings(&["arm64"]));
    set("UILaunchScreen", Value::Dictionary(Dictionary::new()));

    // SDKs from iOS 27 on refuse to launch UIKit apps without the scene life cycle.
    let mut scene = Dictionary::new();
    scene.insert("UISceneConfigurationName".into(), "Default".into());
    scene.insert("UISceneDelegateClassName".into(), "A2ISceneDelegate".into());
    let mut roles = Dictionary::new();
    roles.insert(
        "UIWindowSceneSessionRoleApplication".into(),
        Value::Array(vec![Value::Dictionary(scene)]),
    );
    let mut scenes = Dictionary::new();
    scenes.insert("UIApplicationSupportsMultipleScenes".into(), false.into());
    scenes.insert("UISceneConfigurations".into(), Value::Dictionary(roles));
    set("UIApplicationSceneManifest", Value::Dictionary(scenes));

    let orientations = match m.launcher_orientation() {
        // ActivityInfo.SCREEN_ORIENTATION_*: portrait, sensorPortrait, reversePortrait, userPortrait
        Some(1 | 7 | 9 | 12) => strings(&["UIInterfaceOrientationPortrait"]),
        // landscape, sensorLandscape, reverseLandscape, userLandscape
        Some(0 | 6 | 8 | 11) => strings(&[
            "UIInterfaceOrientationLandscapeLeft",
            "UIInterfaceOrientationLandscapeRight",
        ]),
        _ => strings(&[
            "UIInterfaceOrientationPortrait",
            "UIInterfaceOrientationLandscapeLeft",
            "UIInterfaceOrientationLandscapeRight",
        ]),
    };
    set("UISupportedInterfaceOrientations", orientations);

    let icon_files = |names: &[&str]| {
        let mut primary = Dictionary::new();
        primary.insert("CFBundleIconFiles".into(), strings(names));
        primary.insert("CFBundleIconName".into(), "AppIcon".into());
        let mut icons = Dictionary::new();
        icons.insert("CFBundlePrimaryIcon".into(), Value::Dictionary(primary));
        Value::Dictionary(icons)
    };
    set("CFBundleIcons", icon_files(&["AppIcon60x60"]));
    set(
        "CFBundleIcons~ipad",
        icon_files(&["AppIcon60x60", "AppIcon76x76", "AppIcon83.5x83.5"]),
    );

    // iOS kills an app that touches a protected API without a usage string, so declare one
    // for every Android permission that maps onto such an API.
    for permission in &m.permissions {
        let short = permission.rsplit('.').next().unwrap_or(permission);
        for key in usage_keys(short) {
            set(
                key,
                format!("“{label}” is an Android app that requests the {short} permission.").into(),
            );
        }
        if short == "INTERNET" {
            // Android applies no transport policy of this kind; do not let ATS break WebView loads.
            let mut ats = Dictionary::new();
            ats.insert("NSAllowsArbitraryLoads".into(), true.into());
            set("NSAppTransportSecurity", Value::Dictionary(ats));
        }
    }

    Value::Dictionary(d)
}

fn usage_keys(permission: &str) -> &'static [&'static str] {
    match permission {
        "CAMERA" => &["NSCameraUsageDescription"],
        "RECORD_AUDIO" => &["NSMicrophoneUsageDescription"],
        "ACCESS_FINE_LOCATION" | "ACCESS_COARSE_LOCATION" => {
            &["NSLocationWhenInUseUsageDescription"]
        }
        "ACCESS_BACKGROUND_LOCATION" => &["NSLocationAlwaysAndWhenInUseUsageDescription"],
        "READ_CONTACTS" | "WRITE_CONTACTS" => &["NSContactsUsageDescription"],
        "READ_CALENDAR" | "WRITE_CALENDAR" => &[
            "NSCalendarsUsageDescription",
            "NSCalendarsFullAccessUsageDescription",
        ],
        "BLUETOOTH" | "BLUETOOTH_ADMIN" | "BLUETOOTH_CONNECT" | "BLUETOOTH_SCAN" => {
            &["NSBluetoothAlwaysUsageDescription"]
        }
        "READ_EXTERNAL_STORAGE" | "READ_MEDIA_IMAGES" | "READ_MEDIA_VIDEO" => {
            &["NSPhotoLibraryUsageDescription"]
        }
        "WRITE_EXTERNAL_STORAGE" => &["NSPhotoLibraryAddUsageDescription"],
        "BODY_SENSORS" | "ACTIVITY_RECOGNITION" => &["NSMotionUsageDescription"],
        "USE_BIOMETRIC" | "USE_FINGERPRINT" => &["NSFaceIDUsageDescription"],
        "INTERNET" => &["NSLocalNetworkUsageDescription"],
        _ => &[],
    }
}

/// CFBundleShortVersionString must be up to three dot-separated integers.
fn marketing_version(version_name: Option<&str>) -> String {
    let parts: Vec<String> = version_name
        .unwrap_or_default()
        .split('.')
        .map(|p| {
            p.chars()
                .take_while(char::is_ascii_digit)
                .collect::<String>()
        })
        .take_while(|p| !p.is_empty())
        .take(3)
        // Strip leading zeros and clamp absurd lengths; "007" and 30-digit parts are both invalid.
        .map(|p| {
            p.chars()
                .take(9)
                .collect::<String>()
                .parse::<u64>()
                .unwrap_or(0)
                .to_string()
        })
        .collect();
    if parts.is_empty() {
        "1.0".to_owned()
    } else {
        parts.join(".")
    }
}

/// iOS bundle identifiers allow only alphanumerics, '-' and '.'.
pub fn bundle_id_for(package: &str) -> String {
    package
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '.' || c == '-' {
                c
            } else {
                '-'
            }
        })
        .collect()
}

/// Bundle directory name; must be filesystem- and zip-safe.
pub fn app_dir_name(label: &str) -> PathBuf {
    let clean: String = label
        .chars()
        .filter(|c| !matches!(c, '/' | '\\' | ':' | '\0') && !c.is_control())
        .take(60)
        .collect();
    let clean = clean.trim().trim_matches('.');
    PathBuf::from(format!(
        "{}.app",
        if clean.is_empty() { "App" } else { clean }
    ))
}
