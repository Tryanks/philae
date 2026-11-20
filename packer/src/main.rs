mod apk;
mod arsc;
mod axml;
mod bundle;
mod elf2macho;
mod icon;
mod manifest;
mod sign;

use anyhow::{bail, Context, Result};
use apk::Apk;
use bundle::Target;
use clap::{Parser, Subcommand};
use std::fs::{self, File};
use std::io::{self, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use zip::write::SimpleFileOptions;

#[derive(Parser)]
#[command(
    version,
    about = "Repackage an Android APK as an iOS app that carries the a2i Android runtime"
)]
struct Cli {
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Print what the packer understands about an APK
    Inspect { apk: PathBuf },
    /// Build a signed IPA from an APK
    Pack(PackArgs),
    /// Convert one Android native library (.so) to an iOS dylib (unsigned)
    ConvertSo {
        input: PathBuf,
        #[arg(short, long)]
        output: PathBuf,
        #[arg(short, long, value_enum, default_value = "sim")]
        target: Target,
        /// The bionic compatibility library, to check the library's imports against
        #[arg(long)]
        bionic: Option<PathBuf>,
        /// Other libraries of the same app that this one may import from
        #[arg(long)]
        sibling: Vec<PathBuf>,
    },
}

#[derive(Parser)]
struct PackArgs {
    apk: PathBuf,
    /// Output .ipa path [default: <package>-<target>.ipa in the current directory]
    #[arg(short, long)]
    output: Option<PathBuf>,
    #[arg(short, long, value_enum, default_value = "sim")]
    target: Target,
    /// iOS bundle identifier [default: derived from the Android package name]
    #[arg(long)]
    bundle_id: Option<String>,
    /// Signing identity (name or SHA-1) for device builds [default: first in keychain]
    #[arg(long)]
    identity: Option<String>,
    /// Provisioning profile for device builds [default: an installed one matching the bundle id]
    #[arg(long)]
    profile: Option<PathBuf>,
    /// Directory holding prebuilt runtime hosts (<dir>/<sdk>/A2IHost)
    #[arg(long, env = "PHILAE_RUNTIME")]
    runtime_dir: Option<PathBuf>,
    /// Install after packing: on the booted simulator, or on --device for device builds
    #[arg(long)]
    install: bool,
    /// Launch after installing (implies --install)
    #[arg(long)]
    launch: bool,
    /// Device identifier for --install with --target device (see `xcrun devicectl list devices`)
    #[arg(long)]
    device: Option<String>,
}

fn main() -> Result<()> {
    match Cli::parse().command {
        Cmd::Inspect { apk } => inspect(&apk),
        Cmd::Pack(args) => pack(args),
        Cmd::ConvertSo {
            input,
            output,
            target,
            bionic,
            sibling,
        } => convert_so(&input, &output, target, bionic.as_deref(), &sibling),
    }
}

fn convert_so(
    input: &Path,
    output: &Path,
    target: Target,
    bionic: Option<&Path>,
    siblings: &[PathBuf],
) -> Result<()> {
    let data = fs::read(input).with_context(|| format!("reading {}", input.display()))?;
    let bionic_exports = bionic.map(bundle::bionic_exports).transpose()?;
    let mut sibling_exports = std::collections::BTreeMap::new();
    for path in siblings {
        let name = path
            .file_name()
            .and_then(|n| n.to_str())
            .context("bad sibling path")?
            .to_owned();
        sibling_exports.insert(name, elf2macho::exported_symbols(&fs::read(path)?)?);
    }
    let name = input
        .file_name()
        .and_then(|n| n.to_str())
        .context("bad input path")?;
    let converted = elf2macho::convert(
        &data,
        &elf2macho::Options {
            install_name: &elf2macho::sibling_install_name(name),
            platform: target.platform(),
            bionic: bundle::BIONIC_INSTALL_NAME,
            bionic_exports: bionic_exports.as_ref(),
            siblings: &sibling_exports,
        },
    )
    .with_context(|| format!("converting {}", input.display()))?;
    fs::write(output, &converted.macho).with_context(|| format!("writing {}", output.display()))?;
    println!(
        "{} → {} ({} thread pointer reads rewritten)",
        input.display(),
        output.display(),
        converted.thread_pointer_sites
    );
    if !converted.missing.is_empty() {
        println!(
            "  not provided by the bionic library: {}",
            converted.missing.join(" ")
        );
    }
    Ok(())
}

fn inspect(path: &Path) -> Result<()> {
    let apk = Apk::open(path)?;
    let m = &apk.manifest;
    println!("package:      {}", m.package);
    println!("label:        {}", apk.label());
    println!(
        "version:      {} ({})",
        m.version_name.as_deref().unwrap_or("-"),
        m.version_code
    );
    println!("sdk:          min {} / target {}", m.min_sdk, m.target_sdk);
    println!(
        "application:  {}",
        m.application_class.as_deref().unwrap_or("-")
    );
    println!(
        "launcher:     {}",
        m.launcher_activity.as_deref().unwrap_or("-")
    );
    println!(
        "components:   {} activities, {} services, {} receivers, {} providers",
        m.activities.len(),
        m.services,
        m.receivers,
        m.providers
    );
    println!("dex:          {}", apk.dex_names().join(", "));
    for (abi, libs) in apk.native_libs() {
        println!("native {abi}: {}", libs.join(", "));
    }
    println!("permissions:  {}", m.permissions.len());
    for p in &m.permissions {
        println!("  {p}");
    }
    Ok(())
}

fn pack(args: PackArgs) -> Result<()> {
    let mut apk = Apk::open(&args.apk)?;
    let package = apk.manifest.package.clone();
    let target = args.target;
    let bundle_id = args
        .bundle_id
        .clone()
        .unwrap_or_else(|| bundle::bundle_id_for(&package));

    // Resolve signing inputs before doing any work so that failures are immediate.
    let profile = match target {
        Target::Sim => None,
        Target::Device => Some(resolve_profile(args.profile.as_deref(), &bundle_id)?),
    };

    let output = match args.output {
        Some(o) => o,
        None => PathBuf::from(format!("{package}-{}.ipa", target.sdk())),
    };
    let work = output.with_extension("build");
    reset_work_dir(&work)?;
    let payload = work.join("Payload");
    let app_dir = payload.join(bundle::app_dir_name(&apk.label()));
    fs::create_dir_all(&app_dir)?;

    let runtime_dir = resolve_runtime_dir(args.runtime_dir)?;
    let built = bundle::build(
        &mut apk,
        &args.apk,
        &app_dir,
        target,
        &bundle_id,
        &runtime_dir,
    )?;

    match &profile {
        None => sign::sign_adhoc(&app_dir)?,
        Some(p) => sign::sign_for_device(&app_dir, &bundle_id, args.identity.as_deref(), p)?,
    }
    write_ipa(&payload, &output)?;

    println!("{} → {}", args.apk.display(), output.display());
    println!("  app:        {} ({bundle_id})", built.label);
    println!("  target:     {}", target.sdk());
    println!(
        "  icon:       {}",
        match built.icon {
            icon::IconSource::Adaptive => "adaptive icon layers",
            icon::IconSource::Bitmap => "legacy bitmap",
            icon::IconSource::Placeholder => "placeholder (launcher icon is a vector/XML drawable)",
        }
    );
    println!("  dex:        {} file(s)", built.dex.len());
    match &profile {
        None => println!("  signature:  ad-hoc"),
        Some(p) => println!("  signature:  profile “{}”", p.name),
    }
    for w in &built.warnings {
        println!("  warning:    {w}");
    }

    if args.install || args.launch {
        install(target, &app_dir, args.device.as_deref())?;
        println!("  installed");
        if args.launch {
            launch(target, &bundle_id, args.device.as_deref())?;
        }
    }
    Ok(())
}

fn resolve_profile(explicit: Option<&Path>, bundle_id: &str) -> Result<sign::Profile> {
    if let Some(path) = explicit {
        return sign::Profile::load(path);
    }
    let profiles = sign::installed_profiles();
    let available: Vec<String> = profiles.iter().map(|p| p.app_id.clone()).collect();
    // Prefer the most specific match (an exact id over a wildcard).
    profiles
        .into_iter()
        .filter(|p| p.matches(bundle_id))
        .max_by_key(|p| p.app_id.len())
        .with_context(|| {
            format!(
                "no installed provisioning profile covers bundle id {bundle_id}.\n\
                 Installed profiles cover: {}.\n\
                 Pass --bundle-id with one of those, or --profile with a profile for this id.",
                if available.is_empty() {
                    "(none)".to_owned()
                } else {
                    available.join(", ")
                }
            )
        })
}

fn resolve_runtime_dir(explicit: Option<PathBuf>) -> Result<PathBuf> {
    if let Some(dir) = explicit {
        return Ok(dir);
    }
    // Running from a checkout: target/<profile>/philae → <repo>/out/runtime.
    let exe = std::env::current_exe()?;
    let candidates = [
        exe.ancestors().nth(3).map(|root| root.join("out/runtime")),
        Some(PathBuf::from("out/runtime")),
    ];
    candidates
        .into_iter()
        .flatten()
        .find(|c| c.is_dir())
        .context(
        "runtime directory not found; build it with scripts/build-runtime.sh or pass --runtime-dir",
    )
}

/// Start from an empty work directory, but only ever delete one this tool created.
fn reset_work_dir(work: &Path) -> Result<()> {
    if work.exists() {
        let ours = fs::read_dir(work)?
            .filter_map(|e| e.ok())
            .all(|e| e.file_name() == "Payload");
        if !ours {
            bail!(
                "{} exists and was not created by philae; refusing to overwrite it",
                work.display()
            );
        }
        fs::remove_dir_all(work)?;
    }
    fs::create_dir_all(work).with_context(|| format!("creating {}", work.display()))
}

fn write_ipa(payload: &Path, output: &Path) -> Result<()> {
    let mut files = Vec::new();
    collect_files(payload, &mut files)?;
    files.sort();

    let root = payload.parent().context("payload has no parent")?;
    let mut zip = zip::ZipWriter::new(
        File::create(output).with_context(|| format!("creating {}", output.display()))?,
    );
    for path in files {
        let name = path
            .strip_prefix(root)?
            .to_str()
            .context("non-UTF-8 path in bundle")?
            .to_owned();
        let mode = fs::metadata(&path)?.permissions().mode();
        let options = SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .large_file(true)
            .unix_permissions(if mode & 0o111 != 0 { 0o755 } else { 0o644 });
        zip.start_file(name, options)?;
        io::copy(&mut File::open(&path)?, &mut zip)?;
    }
    zip.finish()?.flush()?;
    Ok(())
}

fn collect_files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        if entry.file_type()?.is_dir() {
            collect_files(&entry.path(), out)?;
        } else {
            out.push(entry.path());
        }
    }
    Ok(())
}

fn install(target: Target, app_dir: &Path, device: Option<&str>) -> Result<()> {
    match target {
        Target::Sim => run(Command::new("xcrun")
            .args(["simctl", "install", "booted"])
            .arg(app_dir)),
        Target::Device => {
            let device =
                device.context("--device <identifier> is required to install on a device")?;
            run(Command::new("xcrun")
                .args(["devicectl", "device", "install", "app", "--device", device])
                .arg(app_dir))
        }
    }
}

fn launch(target: Target, bundle_id: &str, device: Option<&str>) -> Result<()> {
    match target {
        Target::Sim => run(Command::new("xcrun").args(["simctl", "launch", "booted", bundle_id])),
        Target::Device => {
            let device =
                device.context("--device <identifier> is required to launch on a device")?;
            run(Command::new("xcrun").args([
                "devicectl",
                "device",
                "process",
                "launch",
                "--device",
                device,
                bundle_id,
            ]))
        }
    }
}

fn run(cmd: &mut Command) -> Result<()> {
    let out = cmd
        .output()
        .with_context(|| format!("running {:?}", cmd.get_program()))?;
    if !out.status.success() {
        bail!(
            "{:?} failed: {}",
            cmd.get_program(),
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    Ok(())
}
