//! Read access to an APK and the metadata derived from it.

use crate::arsc::ResTable;
use crate::axml::{self, Value};
use crate::manifest::Manifest;
use anyhow::{bail, Context, Result};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::Read;
use std::path::Path;
use zip::ZipArchive;

/// Refuse to inflate single entries larger than this; guards against zip bombs.
const MAX_ENTRY_SIZE: u64 = 2 << 30;

pub struct Apk {
    zip: ZipArchive<File>,
    pub manifest: Manifest,
    pub resources: Option<ResTable>,
}

impl Apk {
    pub fn open(path: &Path) -> Result<Self> {
        let file = File::open(path).with_context(|| format!("cannot open {}", path.display()))?;
        let mut zip =
            ZipArchive::new(file).context("not a zip archive (is this really an APK?)")?;
        if zip.by_name("AndroidManifest.xml").is_err() {
            if zip.file_names().any(|n| n.ends_with(".apk")) {
                bail!("this is a split-APK bundle (.apks/.xapk); only single universal APKs are supported so far");
            }
            bail!("no AndroidManifest.xml inside; not an APK");
        }
        let manifest_xml = read_entry(&mut zip, "AndroidManifest.xml")?;
        let manifest = Manifest::from_axml(
            &axml::parse(&manifest_xml).context("parsing AndroidManifest.xml")?,
        )?;
        let resources = match read_entry(&mut zip, "resources.arsc") {
            Ok(data) => Some(ResTable::parse(data).context("parsing resources.arsc")?),
            Err(_) => None,
        };
        Ok(Self {
            zip,
            manifest,
            resources,
        })
    }

    pub fn read(&mut self, name: &str) -> Result<Vec<u8>> {
        read_entry(&mut self.zip, name)
    }

    /// `classes.dex`, `classes2.dex`, ... in load order.
    pub fn dex_names(&self) -> Vec<String> {
        let mut names: Vec<(u32, String)> = self
            .zip
            .file_names()
            .filter_map(|n| {
                let index = n.strip_prefix("classes")?.strip_suffix(".dex")?;
                let order = if index.is_empty() {
                    1
                } else {
                    index.parse().ok()?
                };
                Some((order, n.to_owned()))
            })
            .collect();
        names.sort();
        names.into_iter().map(|(_, n)| n).collect()
    }

    /// Native libraries grouped by ABI directory (`lib/<abi>/<name>.so`).
    pub fn native_libs(&self) -> BTreeMap<String, Vec<String>> {
        let mut libs: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for name in self.zip.file_names() {
            let Some(rest) = name.strip_prefix("lib/") else {
                continue;
            };
            if let Some((abi, file)) = rest.split_once('/') {
                if !file.contains('/') && file.ends_with(".so") {
                    libs.entry(abi.to_owned())
                        .or_default()
                        .push(file.to_owned());
                }
            }
        }
        libs.values_mut().for_each(|v| v.sort());
        libs
    }

    /// The application label for the default locale.
    pub fn label(&self) -> String {
        let resolved = match &self.manifest.label {
            Some(Value::Str(s)) => Some(s.clone()),
            Some(Value::Ref(id)) => self.resources.as_ref().and_then(|t| t.resolve_string(*id)),
            _ => None,
        };
        resolved
            .map(|s| s.trim().to_owned())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| self.manifest.package.clone())
    }
}

fn read_entry(zip: &mut ZipArchive<File>, name: &str) -> Result<Vec<u8>> {
    let entry = zip
        .by_name(name)
        .with_context(|| format!("{name} not found in APK"))?;
    if entry.size() > MAX_ENTRY_SIZE {
        bail!("{name} is implausibly large ({} bytes)", entry.size());
    }
    let mut data = Vec::with_capacity(entry.size() as usize);
    entry
        .take(MAX_ENTRY_SIZE)
        .read_to_end(&mut data)
        .with_context(|| format!("reading {name} from APK"))?;
    Ok(data)
}
