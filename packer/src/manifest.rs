//! The subset of AndroidManifest.xml the packer and the runtime host need.

use crate::axml::{Element, Value};
use anyhow::{ensure, Context, Result};

// Framework attribute ids (android.R.attr.*).
const A_LABEL: u32 = 0x0101_0001;
const A_ICON: u32 = 0x0101_0002;
const A_NAME: u32 = 0x0101_0003;
const A_ENABLED: u32 = 0x0101_000e;
const A_SCREEN_ORIENTATION: u32 = 0x0101_001e;
const A_TARGET_ACTIVITY: u32 = 0x0101_0202;
const A_MIN_SDK: u32 = 0x0101_020c;
const A_VERSION_CODE: u32 = 0x0101_021b;
const A_VERSION_NAME: u32 = 0x0101_021c;
const A_TARGET_SDK: u32 = 0x0101_0270;
const A_ROUND_ICON: u32 = 0x0101_052c;

#[derive(Debug)]
pub struct Activity {
    pub name: String,
    /// `android:screenOrientation` (ActivityInfo.SCREEN_ORIENTATION_*), if set.
    pub screen_orientation: Option<i64>,
}

#[derive(Debug)]
pub struct Manifest {
    pub package: String,
    pub version_code: i64,
    pub version_name: Option<String>,
    pub min_sdk: i64,
    pub target_sdk: i64,
    pub application_class: Option<String>,
    pub label: Option<Value>,
    pub icon: Option<u32>,
    pub round_icon: Option<u32>,
    pub activities: Vec<Activity>,
    pub services: usize,
    pub receivers: usize,
    pub providers: usize,
    pub permissions: Vec<String>,
    /// Fully-qualified class name of the activity the launcher would start.
    pub launcher_activity: Option<String>,
}

impl Manifest {
    pub fn from_axml(root: &Element) -> Result<Self> {
        ensure!(
            root.name == "manifest",
            "root element is <{}>, not <manifest>",
            root.name
        );
        let package = root
            .attr(0, "package")
            .and_then(Value::as_str)
            .context("manifest has no package attribute")?
            .to_owned();
        let qualify = |name: &str| -> String {
            if let Some(rest) = name.strip_prefix('.') {
                format!("{package}.{rest}")
            } else if name.contains('.') {
                name.to_owned()
            } else {
                format!("{package}.{name}")
            }
        };
        let class_attr = |el: &Element, id: u32, name: &str| -> Option<String> {
            el.attr(id, name).and_then(Value::as_str).map(qualify)
        };
        let as_ref = |v: &Value| match v {
            Value::Ref(id) => Some(*id),
            _ => None,
        };

        let uses_sdk = root.children_named("uses-sdk").next();
        let sdk_attr = |id, name| {
            uses_sdk
                .and_then(|e| e.attr(id, name))
                .and_then(Value::as_int)
        };
        let min_sdk = sdk_attr(A_MIN_SDK, "minSdkVersion").unwrap_or(1);

        let permissions = root
            .children_named("uses-permission")
            .chain(root.children_named("uses-permission-sdk-23"))
            .filter_map(|e| e.attr(A_NAME, "name").and_then(Value::as_str))
            .map(str::to_owned)
            .collect();

        let app = root.children_named("application").next();
        let mut activities = Vec::new();
        let mut launcher: Option<(u8, String)> = None;
        for el in app.iter().flat_map(|a| a.children.iter()) {
            let is_alias = el.name == "activity-alias";
            if el.name != "activity" && !is_alias {
                continue;
            }
            let Some(name) = class_attr(el, A_NAME, "name") else {
                continue;
            };
            if !is_alias {
                activities.push(Activity {
                    name: name.clone(),
                    screen_orientation: el
                        .attr(A_SCREEN_ORIENTATION, "screenOrientation")
                        .and_then(Value::as_int),
                });
            }
            if el.attr(A_ENABLED, "enabled") == Some(&Value::Bool(false)) {
                continue;
            }
            // An alias launches its target; the alias itself has no class.
            let target = if is_alias {
                class_attr(el, A_TARGET_ACTIVITY, "targetActivity").unwrap_or(name)
            } else {
                name
            };
            if let Some(rank) = launcher_rank(el) {
                if launcher.as_ref().is_none_or(|(best, _)| rank < *best) {
                    launcher = Some((rank, target));
                }
            }
        }

        let count = |tag: &str| app.map_or(0, |a| a.children_named(tag).count());
        Ok(Manifest {
            version_code: root
                .attr(A_VERSION_CODE, "versionCode")
                .and_then(Value::as_int)
                .unwrap_or(1),
            version_name: root
                .attr(A_VERSION_NAME, "versionName")
                .and_then(|v| match v {
                    Value::Str(s) => Some(s.clone()),
                    other => other.as_int().map(|i| i.to_string()),
                }),
            min_sdk,
            target_sdk: sdk_attr(A_TARGET_SDK, "targetSdkVersion").unwrap_or(min_sdk),
            application_class: app.and_then(|a| class_attr(a, A_NAME, "name")),
            label: app.and_then(|a| a.attr(A_LABEL, "label")).cloned(),
            icon: app.and_then(|a| a.attr(A_ICON, "icon")).and_then(as_ref),
            round_icon: app
                .and_then(|a| a.attr(A_ROUND_ICON, "roundIcon"))
                .and_then(as_ref),
            activities,
            services: count("service"),
            receivers: count("receiver"),
            providers: count("provider"),
            permissions,
            launcher_activity: launcher.map(|(_, name)| name),
            package,
        })
    }

    pub fn launcher_orientation(&self) -> Option<i64> {
        let launcher = self.launcher_activity.as_deref()?;
        self.activities
            .iter()
            .find(|a| a.name == launcher)?
            .screen_orientation
    }
}

/// How launcher-like an activity's intent filters are; lower is better, `None` is not launchable.
fn launcher_rank(activity: &Element) -> Option<u8> {
    activity
        .children_named("intent-filter")
        .filter_map(|filter| {
            let has = |tag: &str, value: &str| {
                filter
                    .children_named(tag)
                    .any(|e| e.attr(A_NAME, "name").and_then(Value::as_str) == Some(value))
            };
            if !has("action", "android.intent.action.MAIN") {
                None
            } else if has("category", "android.intent.category.LAUNCHER") {
                Some(0)
            } else if has("category", "android.intent.category.LEANBACK_LAUNCHER") {
                Some(1)
            } else {
                Some(2)
            }
        })
        .min()
}
