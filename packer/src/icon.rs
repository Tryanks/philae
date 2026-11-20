//! Turn the APK's launcher icon into an opaque square image usable as an iOS app icon.
//!
//! Preference order: adaptive icon (background + foreground layers), then the densest
//! legacy bitmap, then a generated placeholder. Layers that are themselves XML drawables
//! (vectors, shapes, insets) cannot be rasterised yet and fall through to the next option.

use crate::apk::Apk;
use crate::arsc::{Config, DENSITY_ANY, DENSITY_NONE};
use crate::axml::{self, Value};
use image::imageops::{self, FilterType};
use image::{Rgb, RgbImage, Rgba, RgbaImage};

const A_DRAWABLE: u32 = 0x0101_0199;
const MASTER: u32 = 1024;

pub enum IconSource {
    Adaptive,
    Bitmap,
    Placeholder,
}

pub fn render(apk: &mut Apk) -> (RgbImage, IconSource) {
    let ids: Vec<u32> = [apk.manifest.icon, apk.manifest.round_icon]
        .into_iter()
        .flatten()
        .collect();
    for id in &ids {
        if let Some(img) = adaptive(apk, *id) {
            return (flatten(&img), IconSource::Adaptive);
        }
    }
    for id in &ids {
        if let Some(bitmap) = best_bitmap(apk, *id) {
            let scaled = imageops::resize(&bitmap, MASTER, MASTER, FilterType::Lanczos3);
            return (flatten(&scaled), IconSource::Bitmap);
        }
    }
    (placeholder(&apk.manifest.package), IconSource::Placeholder)
}

/// File-backed and colour values of a drawable resource, best candidate first.
fn candidates(apk: &Apk, res_id: u32) -> Vec<Value> {
    let Some(table) = apk.resources.as_ref() else {
        return Vec::new();
    };
    let mut values = table.lookup_resolved(res_id);
    values.retain(|(c, _)| !c.night);
    values.sort_by_key(|(c, _)| std::cmp::Reverse(density_rank(c)));
    values.into_iter().map(|(_, v)| v).collect()
}

fn density_rank(c: &Config) -> u32 {
    match c.density {
        // Density-independent files are usually XML; rank bitmaps among them like xhdpi.
        0 | DENSITY_ANY | DENSITY_NONE => 320,
        d => d as u32,
    }
}

fn best_bitmap(apk: &mut Apk, res_id: u32) -> Option<RgbaImage> {
    candidates(apk, res_id).into_iter().find_map(|v| match v {
        Value::Str(path) => decode(apk, &path),
        _ => None,
    })
}

fn decode(apk: &mut Apk, path: &str) -> Option<RgbaImage> {
    let data = apk.read(path).ok()?;
    // Decoding sniffs the magic bytes, so obfuscated extension-less paths work too;
    // XML drawables simply fail to decode.
    image::load_from_memory(&data).ok().map(|i| i.to_rgba8())
}

enum Layer {
    Color(u32),
    Image(RgbaImage),
}

fn layer(apk: &mut Apk, value: &Value) -> Option<Layer> {
    match value {
        Value::Color(argb) => Some(Layer::Color(*argb)),
        Value::Ref(id) => candidates(apk, *id).into_iter().find_map(|v| match v {
            Value::Color(argb) => Some(Layer::Color(argb)),
            Value::Str(path) => decode(apk, &path).map(Layer::Image),
            _ => None,
        }),
        _ => None,
    }
}

fn adaptive(apk: &mut Apk, res_id: u32) -> Option<RgbaImage> {
    let xml = candidates(apk, res_id).into_iter().find_map(|v| {
        let Value::Str(path) = v else { return None };
        let root = axml::parse(&apk.read(&path).ok()?).ok()?;
        (root.name == "adaptive-icon").then_some(root)
    })?;
    let drawable = |tag: &str| {
        xml.children_named(tag)
            .next()?
            .attr(A_DRAWABLE, "drawable")
            .cloned()
    };
    let foreground = layer(apk, &drawable("foreground")?)?;
    // A missing or unrenderable background still gives a usable icon on white.
    let background = drawable("background")
        .and_then(|v| layer(apk, &v))
        .unwrap_or(Layer::Color(0xffff_ffff));

    // Adaptive layers are 108dp with the middle 72dp visible, so draw them at 1.5x and crop.
    let full = MASTER * 108 / 72;
    let offset = -i64::from((full - MASTER) / 2);
    let mut canvas = RgbaImage::from_pixel(MASTER, MASTER, Rgba([255, 255, 255, 255]));
    for l in [background, foreground] {
        match l {
            Layer::Color(argb) => {
                let [a, r, g, b] = argb.to_be_bytes();
                let fill = RgbaImage::from_pixel(MASTER, MASTER, Rgba([r, g, b, a]));
                imageops::overlay(&mut canvas, &fill, 0, 0);
            }
            Layer::Image(img) => {
                let scaled = imageops::resize(&img, full, full, FilterType::Lanczos3);
                imageops::overlay(&mut canvas, &scaled, offset, offset);
            }
        }
    }
    Some(canvas)
}

/// iOS rejects icons with an alpha channel; composite onto white.
fn flatten(img: &RgbaImage) -> RgbImage {
    let mut canvas = RgbaImage::from_pixel(img.width(), img.height(), Rgba([255, 255, 255, 255]));
    imageops::overlay(&mut canvas, img, 0, 0);
    image::DynamicImage::ImageRgba8(canvas).to_rgb8()
}

fn placeholder(package: &str) -> RgbImage {
    // Stable per-package colour so different apps are distinguishable on the home screen.
    let hash = package.bytes().fold(0x811c_9dc5u32, |h, b| {
        (h ^ b as u32).wrapping_mul(0x0100_0193)
    });
    let [r, g, b, _] = hash.to_le_bytes();
    let tone = |c: u8| 64 + c / 2;
    let (top, bottom) = (
        [tone(r), tone(g), tone(b)],
        [tone(r) / 2, tone(g) / 2, tone(b) / 2],
    );
    RgbImage::from_fn(MASTER, MASTER, |_, y| {
        let t = y as f32 / MASTER as f32;
        let mix = |i: usize| (top[i] as f32 * (1.0 - t) + bottom[i] as f32 * t) as u8;
        Rgb([mix(0), mix(1), mix(2)])
    })
}

pub fn scaled(master: &RgbImage, size: u32) -> RgbImage {
    imageops::resize(master, size, size, FilterType::Lanczos3)
}
