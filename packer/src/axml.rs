//! Android binary XML (AXML) and the string pool shared with resources.arsc.
//!
//! Everything here reads attacker-controlled bytes, so all access is bounds-checked
//! and malformed input yields an error instead of a panic.

use anyhow::{bail, ensure, Context, Result};

pub const CHUNK_STRING_POOL: u16 = 0x0001;
const CHUNK_XML: u16 = 0x0003;
const CHUNK_XML_START_ELEMENT: u16 = 0x0102;
const CHUNK_XML_END_ELEMENT: u16 = 0x0103;
const CHUNK_XML_RESOURCE_MAP: u16 = 0x0180;

/// Bounds-checked little-endian reader over a byte slice.
#[derive(Clone, Copy)]
pub struct Rd<'a>(pub &'a [u8]);

impl<'a> Rd<'a> {
    pub fn slice(&self, off: usize, len: usize) -> Result<&'a [u8]> {
        off.checked_add(len)
            .and_then(|end| self.0.get(off..end))
            .with_context(|| format!("read of {len} bytes at {off:#x} is out of bounds"))
    }
    pub fn u8(&self, off: usize) -> Result<u8> {
        Ok(self.slice(off, 1)?[0])
    }
    pub fn u16(&self, off: usize) -> Result<u16> {
        let s = self.slice(off, 2)?;
        Ok(u16::from_le_bytes([s[0], s[1]]))
    }
    pub fn u32(&self, off: usize) -> Result<u32> {
        let s = self.slice(off, 4)?;
        Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
}

pub struct StringPool {
    strings: Vec<String>,
}

impl StringPool {
    pub fn parse(data: &[u8], start: usize) -> Result<Self> {
        let r = Rd(data);
        ensure!(
            r.u16(start)? == CHUNK_STRING_POOL,
            "not a string pool chunk"
        );
        let header_size = r.u16(start + 2)? as usize;
        let size = r.u32(start + 4)? as usize;
        let count = r.u32(start + 8)? as usize;
        let flags = r.u32(start + 16)?;
        let strings_start = r.u32(start + 20)? as usize;
        let utf8 = flags & 0x100 != 0;
        ensure!(
            count.checked_mul(4).is_some_and(|n| n <= size),
            "string pool count exceeds chunk size"
        );
        let pool = Rd(r.slice(start, size)?);

        let mut strings = Vec::with_capacity(count);
        for i in 0..count {
            let off = pool.u32(header_size + 4 * i)? as usize;
            let s = if utf8 {
                decode_utf8(pool, strings_start + off)
            } else {
                decode_utf16(pool, strings_start + off)
            };
            // A single corrupt string should not make the whole file unreadable.
            strings.push(s.unwrap_or_default());
        }
        Ok(Self { strings })
    }

    pub fn get(&self, idx: u32) -> Option<&str> {
        self.strings.get(idx as usize).map(String::as_str)
    }
}

fn decode_utf8(r: Rd, mut p: usize) -> Result<String> {
    // Two length prefixes: UTF-16 length (ignored), then UTF-8 byte length.
    let mut len = 0usize;
    for _ in 0..2 {
        let b0 = r.u8(p)? as usize;
        p += 1;
        len = b0;
        if b0 & 0x80 != 0 {
            len = ((b0 & 0x7f) << 8) | r.u8(p)? as usize;
            p += 1;
        }
    }
    Ok(String::from_utf8_lossy(r.slice(p, len)?).into_owned())
}

fn decode_utf16(r: Rd, mut p: usize) -> Result<String> {
    let mut len = r.u16(p)? as usize;
    p += 2;
    if len & 0x8000 != 0 {
        len = ((len & 0x7fff) << 16) | r.u16(p)? as usize;
        p += 2;
    }
    let bytes = r.slice(p, len.checked_mul(2).context("string too long")?)?;
    let units: Vec<u16> = bytes
        .as_chunks::<2>()
        .0
        .iter()
        .map(|c| u16::from_le_bytes(*c))
        .collect();
    Ok(String::from_utf16_lossy(&units))
}

/// A typed value as stored in `Res_value` (attributes and resource table entries).
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Str(String),
    /// Reference to a resource id (`@type/name`).
    Ref(u32),
    Int(i32),
    Bool(bool),
    /// 0xAARRGGBB.
    Color(u32),
    Other {
        data_type: u8,
        data: u32,
    },
}

impl Value {
    pub fn from_typed(data_type: u8, data: u32, pool: &StringPool) -> Self {
        match data_type {
            0x03 => Value::Str(pool.get(data).unwrap_or_default().to_owned()),
            0x01 | 0x07 => Value::Ref(data),
            0x10 | 0x11 => Value::Int(data as i32),
            0x12 => Value::Bool(data != 0),
            0x1c..=0x1f => Value::Color(data),
            _ => Value::Other { data_type, data },
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_int(&self) -> Option<i64> {
        match self {
            Value::Int(i) => Some(*i as i64),
            Value::Str(s) => s.parse().ok(),
            _ => None,
        }
    }
}

#[derive(Debug)]
pub struct Attr {
    pub name: String,
    /// Framework attribute id (e.g. 0x01010003 for `android:name`), when the file has a resource map.
    pub res_id: Option<u32>,
    pub value: Value,
}

#[derive(Debug, Default)]
pub struct Element {
    pub name: String,
    pub attrs: Vec<Attr>,
    pub children: Vec<Element>,
}

impl Element {
    /// Look an attribute up by framework id, falling back to its plain name.
    /// Obfuscators often blank the name strings, so the id is the reliable key.
    pub fn attr(&self, res_id: u32, name: &str) -> Option<&Value> {
        self.attrs
            .iter()
            .find(|a| a.res_id == Some(res_id))
            .or_else(|| {
                self.attrs
                    .iter()
                    .find(|a| a.res_id.is_none() && a.name == name)
            })
            .map(|a| &a.value)
    }

    pub fn children_named<'a>(&'a self, name: &'a str) -> impl Iterator<Item = &'a Element> {
        self.children.iter().filter(move |c| c.name == name)
    }
}

pub fn parse(data: &[u8]) -> Result<Element> {
    let r = Rd(data);
    ensure!(r.u16(0)? == CHUNK_XML, "not a binary XML file");
    let total = (r.u32(4)? as usize).min(data.len());
    let mut pos = r.u16(2)? as usize;

    let mut pool: Option<StringPool> = None;
    let mut res_map: Vec<u32> = Vec::new();
    let mut stack: Vec<Element> = Vec::new();
    let mut root: Option<Element> = None;

    while pos + 8 <= total {
        let ty = r.u16(pos)?;
        let header_size = r.u16(pos + 2)? as usize;
        let size = r.u32(pos + 4)? as usize;
        ensure!(
            size >= 8 && pos + size <= total,
            "corrupt XML chunk at {pos:#x}"
        );

        match ty {
            CHUNK_STRING_POOL => pool = Some(StringPool::parse(data, pos)?),
            CHUNK_XML_RESOURCE_MAP => {
                let n = size.saturating_sub(header_size) / 4;
                res_map = (0..n)
                    .map(|i| r.u32(pos + header_size + 4 * i))
                    .collect::<Result<_>>()?;
            }
            CHUNK_XML_START_ELEMENT => {
                let pool = pool.as_ref().context("XML element before string pool")?;
                let body = pos + header_size;
                let name = pool.get(r.u32(body + 4)?).unwrap_or_default().to_owned();
                let attr_start = r.u16(body + 8)? as usize;
                let attr_size = (r.u16(body + 10)? as usize).max(20);
                let attr_count = r.u16(body + 12)? as usize;

                let mut attrs = Vec::with_capacity(attr_count);
                for i in 0..attr_count {
                    let a = body + attr_start + i * attr_size;
                    let name_idx = r.u32(a + 4)?;
                    let data_type = r.u8(a + 15)?;
                    let data = r.u32(a + 16)?;
                    attrs.push(Attr {
                        name: pool.get(name_idx).unwrap_or_default().to_owned(),
                        res_id: res_map
                            .get(name_idx as usize)
                            .copied()
                            .filter(|id| *id != 0),
                        value: Value::from_typed(data_type, data, pool),
                    });
                }
                stack.push(Element {
                    name,
                    attrs,
                    children: Vec::new(),
                });
            }
            CHUNK_XML_END_ELEMENT => {
                let el = stack.pop().context("unbalanced XML end element")?;
                match stack.last_mut() {
                    Some(parent) => parent.children.push(el),
                    None => root = Some(el),
                }
            }
            _ => {}
        }
        pos += size;
    }

    match root {
        Some(root) => Ok(root),
        None => bail!("binary XML has no root element"),
    }
}
