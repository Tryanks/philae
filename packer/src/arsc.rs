//! resources.arsc reader: just enough to resolve a resource id to its per-configuration values.
//!
//! Entries are decoded on demand; parsing only indexes where each type chunk lives.

use crate::axml::{Rd, StringPool, Value, CHUNK_STRING_POOL};
use anyhow::{ensure, Result};

const CHUNK_TABLE: u16 = 0x0002;
const CHUNK_PACKAGE: u16 = 0x0200;
const CHUNK_TYPE: u16 = 0x0201;

const TYPE_FLAG_SPARSE: u8 = 0x01;
const TYPE_FLAG_OFFSET16: u8 = 0x02;
const ENTRY_FLAG_COMPLEX: u16 = 0x0001;
const ENTRY_FLAG_COMPACT: u16 = 0x0008;

pub const DENSITY_ANY: u16 = 0xfffe;
pub const DENSITY_NONE: u16 = 0xffff;

/// The parts of `ResTable_config` the packer cares about.
#[derive(Debug, Clone, Copy)]
pub struct Config {
    pub density: u16,
    pub language: [u8; 2],
    pub night: bool,
    /// Number of non-zero qualifier bytes; 0 means the unqualified default configuration.
    pub specificity: usize,
}

#[derive(Debug, Clone)]
pub enum ResValue {
    Simple(Value),
    /// Bag resources (styles, arrays, plurals) — not needed by the packer.
    Complex,
}

struct TypeChunk {
    type_id: u8,
    flags: u8,
    config: Config,
    entry_count: usize,
    /// Absolute offsets into the table data.
    offsets_at: usize,
    entries_at: usize,
    end: usize,
}

struct Package {
    id: u8,
    chunks: Vec<TypeChunk>,
}

pub struct ResTable {
    data: Vec<u8>,
    strings: StringPool,
    packages: Vec<Package>,
}

impl ResTable {
    pub fn parse(data: Vec<u8>) -> Result<Self> {
        let r = Rd(&data);
        ensure!(r.u16(0)? == CHUNK_TABLE, "not a resource table");
        let total = (r.u32(4)? as usize).min(data.len());
        let mut pos = r.u16(2)? as usize;

        let mut strings = None;
        let mut packages = Vec::new();
        while pos + 8 <= total {
            let ty = r.u16(pos)?;
            let size = r.u32(pos + 4)? as usize;
            ensure!(
                size >= 8 && pos + size <= total,
                "corrupt table chunk at {pos:#x}"
            );
            match ty {
                CHUNK_STRING_POOL if strings.is_none() => {
                    strings = Some(StringPool::parse(&data, pos)?)
                }
                CHUNK_PACKAGE => packages.push(parse_package(r, pos, pos + size)?),
                _ => {}
            }
            pos += size;
        }
        let strings = strings.unwrap_or(StringPool::parse(&EMPTY_POOL, 0)?);
        Ok(Self {
            data,
            strings,
            packages,
        })
    }

    /// Every (configuration, value) pair defined for `res_id`.
    pub fn lookup(&self, res_id: u32) -> Vec<(Config, ResValue)> {
        let (pkg_id, type_id, entry) = ((res_id >> 24) as u8, (res_id >> 16) as u8, res_id as u16);
        let mut out = Vec::new();
        for pkg in self.packages.iter().filter(|p| p.id == pkg_id) {
            for chunk in pkg.chunks.iter().filter(|c| c.type_id == type_id) {
                if let Ok(Some(v)) = self.read_entry(chunk, entry) {
                    out.push((chunk.config, v));
                }
            }
        }
        out
    }

    /// Like `lookup`, but follows `@ref` chains so the result holds only concrete values.
    pub fn lookup_resolved(&self, res_id: u32) -> Vec<(Config, Value)> {
        let mut out = Vec::new();
        self.resolve_into(res_id, None, 0, &mut out);
        out
    }

    fn resolve_into(
        &self,
        res_id: u32,
        outer: Option<Config>,
        depth: u32,
        out: &mut Vec<(Config, Value)>,
    ) {
        if depth > 8 {
            return;
        }
        for (config, value) in self.lookup(res_id) {
            // Keep the configuration of the outermost reference: that is what was selected on.
            let config = outer.unwrap_or(config);
            match value {
                ResValue::Simple(Value::Ref(next)) if next != 0 => {
                    self.resolve_into(next, Some(config), depth + 1, out)
                }
                ResValue::Simple(v) => out.push((config, v)),
                ResValue::Complex => {}
            }
        }
    }

    /// Resolve a string resource for the default locale.
    pub fn resolve_string(&self, res_id: u32) -> Option<String> {
        self.lookup_resolved(res_id)
            .into_iter()
            .filter_map(|(c, v)| match v {
                Value::Str(s) => Some((c, s)),
                _ => None,
            })
            .min_by_key(|(c, _)| (c.language != [0, 0], c.specificity))
            .map(|(_, s)| s)
    }

    fn read_entry(&self, chunk: &TypeChunk, entry: u16) -> Result<Option<ResValue>> {
        let r = Rd(&self.data);
        let none32 = u32::MAX as usize;
        let offset = if chunk.flags & TYPE_FLAG_SPARSE != 0 {
            let mut found = none32;
            for i in 0..chunk.entry_count {
                if r.u16(chunk.offsets_at + 4 * i)? == entry {
                    found = r.u16(chunk.offsets_at + 4 * i + 2)? as usize * 4;
                    break;
                }
            }
            found
        } else if entry as usize >= chunk.entry_count {
            none32
        } else if chunk.flags & TYPE_FLAG_OFFSET16 != 0 {
            match r.u16(chunk.offsets_at + 2 * entry as usize)? {
                0xffff => none32,
                o => o as usize * 4,
            }
        } else {
            r.u32(chunk.offsets_at + 4 * entry as usize)? as usize
        };
        if offset == none32 {
            return Ok(None);
        }

        let at = chunk.entries_at + offset;
        ensure!(at + 8 <= chunk.end, "resource entry out of chunk bounds");
        let size = r.u16(at)? as usize;
        let flags = r.u16(at + 2)?;
        if flags & ENTRY_FLAG_COMPACT != 0 {
            // Compact entry: the data type lives in the high byte of `flags`, data follows inline.
            let v = Value::from_typed((flags >> 8) as u8, r.u32(at + 4)?, &self.strings);
            return Ok(Some(ResValue::Simple(v)));
        }
        if flags & ENTRY_FLAG_COMPLEX != 0 {
            return Ok(Some(ResValue::Complex));
        }
        let v = at + size;
        ensure!(v + 8 <= chunk.end, "resource value out of chunk bounds");
        Ok(Some(ResValue::Simple(Value::from_typed(
            r.u8(v + 3)?,
            r.u32(v + 4)?,
            &self.strings,
        ))))
    }
}

/// A valid, empty string pool chunk, for tables that (illegally) have none.
const EMPTY_POOL: [u8; 28] = [
    0x01, 0x00, 0x1c, 0x00, 0x1c, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1c, 0, 0,
    0, 0, 0, 0, 0,
];

fn parse_package(r: Rd, start: usize, end: usize) -> Result<Package> {
    let header_size = r.u16(start + 2)? as usize;
    let id = r.u32(start + 8)? as u8;
    let mut chunks = Vec::new();
    let mut pos = start + header_size;
    while pos + 8 <= end {
        let ty = r.u16(pos)?;
        let chunk_header = r.u16(pos + 2)? as usize;
        let size = r.u32(pos + 4)? as usize;
        ensure!(
            size >= 8 && pos + size <= end,
            "corrupt package chunk at {pos:#x}"
        );
        if ty == CHUNK_TYPE {
            let config_at = pos + 20;
            let config_size = r.u32(config_at)? as usize;
            chunks.push(TypeChunk {
                type_id: r.u8(pos + 8)?,
                flags: r.u8(pos + 9)?,
                config: parse_config(r.slice(config_at, config_size.max(4))?),
                entry_count: r.u32(pos + 12)? as usize,
                offsets_at: pos + chunk_header,
                entries_at: pos + r.u32(pos + 16)? as usize,
                end: pos + size,
            });
        }
        pos += size;
    }
    Ok(Package { id, chunks })
}

fn parse_config(c: &[u8]) -> Config {
    let byte = |i: usize| c.get(i).copied().unwrap_or(0);
    Config {
        density: u16::from_le_bytes([byte(14), byte(15)]),
        language: [byte(8), byte(9)],
        // uiMode: UI_MODE_NIGHT_MASK = 0x30, UI_MODE_NIGHT_YES = 0x20.
        night: byte(29) & 0x30 == 0x20,
        // sdkVersion (bytes 24..28) is implied by other qualifiers, so it does not count.
        specificity: c
            .iter()
            .enumerate()
            .skip(4)
            .filter(|(i, b)| !(24..28).contains(i) && **b != 0)
            .count(),
    }
}
