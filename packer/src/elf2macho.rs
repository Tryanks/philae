//! Converts an Android native library (an ELF shared object for arm64) into an iOS dylib.
//!
//! Both formats hold the same thing, arm64 machine code plus the information a dynamic
//! linker needs, so the code itself is carried over untouched and only the container is
//! rewritten: the ELF image is placed, one page up, inside Mach-O segments; its dynamic
//! relocations become dyld rebase and bind records; its exported symbols become a Mach-O
//! export trie; its initializer array becomes a `__mod_init_func` section.
//!
//! What the code expects from its surroundings is Android's, not iOS's:
//!
//!  - Imports are functions of Android's C library and friends. They are bound to the
//!    runtime's bionic compatibility library, under the name `bionic_<name>`, where each
//!    one has the calling convention and data layouts Android code expects.
//!  - Android code reads its thread pointer register (TPIDR_EL0) directly, in every
//!    function with a stack protector, to fetch the stack guard from a fixed slot. iOS
//!    does not keep that register. Each such instruction is replaced by a jump to a few
//!    instructions, appended to the image, that load the address of a block laid out like
//!    Android's thread slots instead.
//!
//! Requirements on the input, checked here: 16 KiB segment alignment (iOS pages; Android
//! requires the same of current apps), no text relocations, no ELF thread-local storage.

use anyhow::{bail, ensure, Context, Result};
use std::collections::{BTreeMap, BTreeSet};

/// iOS page size, and the amount the ELF image is shifted to make room for the Mach-O header.
const PAGE: u64 = 0x4000;
const SHIFT: u64 = PAGE;

/// Offset of the stack guard in Android's thread slots (TLS_SLOT_STACK_GUARD = 5).
const STACK_GUARD_SLOT_OFFSET: usize = 5 * 8;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Platform {
    Ios,
    IosSimulator,
}

pub struct Options<'a> {
    /// Install name of the output, e.g. `@rpath/libfoo.dylib`.
    pub install_name: &'a str,
    pub platform: Platform,
    /// Install name of the bionic compatibility library.
    pub bionic: &'a str,
    /// What the bionic library exports (without the `bionic_` prefix). `None` skips the check.
    pub bionic_exports: Option<&'a BTreeSet<String>>,
    /// The app's other native libraries: ELF file name (`libbar.so`) to exported symbols.
    pub siblings: &'a BTreeMap<String, BTreeSet<String>>,
}

pub struct Converted {
    pub macho: Vec<u8>,
    /// Imports the bionic library does not provide; calling one aborts.
    pub missing: Vec<String>,
    pub thread_pointer_sites: usize,
}

// ---------------------------------------------------------------------------------------
// ELF
// ---------------------------------------------------------------------------------------

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_TLS: u32 = 7;
const PF_X: u32 = 1;
const PF_W: u32 = 2;

const DT_NEEDED: i64 = 1;
const DT_PLTRELSZ: i64 = 2;
const DT_HASH: i64 = 4;
const DT_STRTAB: i64 = 5;
const DT_SYMTAB: i64 = 6;
const DT_RELA: i64 = 7;
const DT_RELASZ: i64 = 8;
const DT_INIT: i64 = 12;
const DT_JMPREL: i64 = 23;
const DT_INIT_ARRAY: i64 = 25;
const DT_INIT_ARRAYSZ: i64 = 27;
const DT_RELRSZ: i64 = 35;
const DT_RELR: i64 = 36;
const DT_GNU_HASH: i64 = 0x6fff_fef5;
const DT_ANDROID_REL: i64 = 0x6000_000f;
const DT_ANDROID_RELA: i64 = 0x6000_0011;

const R_AARCH64_ABS64: u32 = 257;
const R_AARCH64_GLOB_DAT: u32 = 1025;
const R_AARCH64_JUMP_SLOT: u32 = 1026;
const R_AARCH64_RELATIVE: u32 = 1027;

const STB_WEAK: u8 = 2;
const STB_LOCAL: u8 = 0;

#[derive(Clone, Copy)]
struct Load {
    vaddr: u64,
    memsz: u64,
    filesz: u64,
    offset: u64,
    flags: u32,
}

struct Symbol {
    name: String,
    value: u64,
    defined: bool,
    binding: u8,
}

struct Elf<'a> {
    data: &'a [u8],
    loads: Vec<Load>,
    needed: Vec<String>,
    symbols: Vec<Symbol>,
    /// (address, symbol index, addend, type)
    relocations: Vec<(u64, u32, i64, u32)>,
    /// Addresses relocated by the base address (DT_RELR).
    relative: Vec<u64>,
    init_array: Option<(u64, u64)>,
    has_init: bool,
}

fn u16_at(d: &[u8], o: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(
        d.get(o..o + 2).context("truncated ELF")?.try_into()?,
    ))
}
fn u32_at(d: &[u8], o: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(
        d.get(o..o + 4).context("truncated ELF")?.try_into()?,
    ))
}
fn u64_at(d: &[u8], o: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(
        d.get(o..o + 8).context("truncated ELF")?.try_into()?,
    ))
}

impl<'a> Elf<'a> {
    fn parse(data: &'a [u8]) -> Result<Self> {
        ensure!(
            data.len() > 64 && &data[..4] == b"\x7fELF",
            "not an ELF file"
        );
        ensure!(
            data[4] == 2 && data[5] == 1,
            "not a 64-bit little-endian ELF file"
        );
        ensure!(u16_at(data, 16)? == 3, "not a shared object");
        ensure!(u16_at(data, 18)? == 183, "not an arm64 library");
        let phoff = u64_at(data, 0x20)? as usize;
        let phnum = u16_at(data, 0x38)? as usize;

        let mut loads = Vec::new();
        let mut dynamic = None;
        for i in 0..phnum {
            let p = phoff + i * 56;
            let kind = u32_at(data, p)?;
            let load = Load {
                flags: u32_at(data, p + 4)?,
                offset: u64_at(data, p + 8)?,
                vaddr: u64_at(data, p + 16)?,
                filesz: u64_at(data, p + 32)?,
                memsz: u64_at(data, p + 40)?,
            };
            match kind {
                PT_LOAD => {
                    ensure!(
                        u64_at(data, p + 48)? % PAGE == 0,
                        "segments are aligned to less than 16 KiB; the library must be rebuilt \
                         with 16 KiB page support"
                    );
                    loads.push(load);
                }
                PT_DYNAMIC => dynamic = Some(load),
                PT_TLS => {
                    bail!("the library uses ELF thread-local storage, which is not supported yet")
                }
                _ => {}
            }
        }
        ensure!(!loads.is_empty(), "no loadable segments");
        let dynamic = dynamic.context("no dynamic section")?;

        let mut elf = Elf {
            data,
            loads,
            needed: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            relative: Vec::new(),
            init_array: None,
            has_init: false,
        };

        let mut tags: BTreeMap<i64, Vec<u64>> = BTreeMap::new();
        for i in 0..(dynamic.filesz as usize / 16) {
            let o = dynamic.offset as usize + i * 16;
            let tag = u64_at(data, o)? as i64;
            if tag == 0 {
                break;
            }
            tags.entry(tag).or_default().push(u64_at(data, o + 8)?);
        }
        let tag = |t: i64| tags.get(&t).map(|v| v[0]);
        ensure!(
            tag(DT_ANDROID_REL).is_none() && tag(DT_ANDROID_RELA).is_none(),
            "Android packed relocations are not supported yet"
        );

        let strtab = elf.file_offset(tag(DT_STRTAB).context("no string table")?)?;
        let string = |offset: usize| -> Result<String> {
            let bytes = data.get(strtab + offset..).context("bad string offset")?;
            let end = bytes
                .iter()
                .position(|&b| b == 0)
                .context("unterminated string")?;
            Ok(String::from_utf8_lossy(&bytes[..end]).into_owned())
        };
        for &n in tags.get(&DT_NEEDED).map(Vec::as_slice).unwrap_or(&[]) {
            elf.needed.push(string(n as usize)?);
        }

        let symtab = elf.file_offset(tag(DT_SYMTAB).context("no symbol table")?)?;
        let count = elf.symbol_count(tag(DT_HASH), tag(DT_GNU_HASH))?;
        for i in 0..count {
            let o = symtab + i * 24;
            let info = *data.get(o + 4).context("truncated symbol table")?;
            elf.symbols.push(Symbol {
                name: string(u32_at(data, o)? as usize)?,
                value: u64_at(data, o + 8)?,
                defined: u16_at(data, o + 6)? != 0,
                binding: info >> 4,
            });
        }

        for (table, size) in [(DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)] {
            let (Some(address), Some(size)) = (tag(table), tag(size)) else {
                continue;
            };
            let start = elf.file_offset(address)?;
            for i in 0..(size as usize / 24) {
                let o = start + i * 24;
                let info = u64_at(data, o + 8)?;
                elf.relocations.push((
                    u64_at(data, o)?,
                    (info >> 32) as u32,
                    u64_at(data, o + 16)? as i64,
                    info as u32,
                ));
            }
        }
        if let (Some(address), Some(size)) = (tag(DT_RELR), tag(DT_RELRSZ)) {
            // A compact list of addresses to relocate: an address, then bitmaps of which
            // of the following 63 words are relocated too.
            let start = elf.file_offset(address)?;
            let mut next = 0u64;
            for i in 0..(size as usize / 8) {
                let entry = u64_at(data, start + i * 8)?;
                if entry & 1 == 0 {
                    elf.relative.push(entry);
                    next = entry + 8;
                } else {
                    for bit in 0..63 {
                        if entry >> (bit + 1) & 1 == 1 {
                            elf.relative.push(next + bit * 8);
                        }
                    }
                    next += 63 * 8;
                }
            }
        }
        if let (Some(address), Some(size)) = (tag(DT_INIT_ARRAY), tag(DT_INIT_ARRAYSZ)) {
            if size > 0 {
                elf.init_array = Some((address, size));
            }
        }
        elf.has_init = tag(DT_INIT).is_some();
        Ok(elf)
    }

    fn file_offset(&self, vaddr: u64) -> Result<usize> {
        self.loads
            .iter()
            .find(|l| vaddr >= l.vaddr && vaddr < l.vaddr + l.filesz)
            .map(|l| (l.offset + vaddr - l.vaddr) as usize)
            .with_context(|| format!("address {vaddr:#x} is not in the file"))
    }

    /// The number of dynamic symbols, which only the hash tables record.
    fn symbol_count(&self, hash: Option<u64>, gnu_hash: Option<u64>) -> Result<usize> {
        let d = self.data;
        if let Some(hash) = hash {
            return Ok(u32_at(d, self.file_offset(hash)? + 4)? as usize);
        }
        let gnu = self.file_offset(gnu_hash.context("no symbol hash table")?)?;
        let nbuckets = u32_at(d, gnu)? as usize;
        let symoffset = u32_at(d, gnu + 4)? as usize;
        let bloom_size = u32_at(d, gnu + 8)? as usize;
        let buckets = gnu + 16 + bloom_size * 8;
        let chains = buckets + nbuckets * 4;
        let mut last = 0usize;
        for i in 0..nbuckets {
            last = last.max(u32_at(d, buckets + i * 4)? as usize);
        }
        if last < symoffset {
            return Ok(symoffset);
        }
        // Walk the last chain to its end (marked by the low bit).
        loop {
            if u32_at(d, chains + (last - symoffset) * 4)? & 1 == 1 {
                return Ok(last + 1);
            }
            last += 1;
        }
    }
}

/// The names a library exports, for resolving its siblings' imports.
pub fn exported_symbols(data: &[u8]) -> Result<BTreeSet<String>> {
    let elf = Elf::parse(data)?;
    Ok(elf
        .symbols
        .iter()
        .filter(|s| s.defined && s.binding != STB_LOCAL && !s.name.is_empty())
        .map(|s| s.name.clone())
        .collect())
}

// ---------------------------------------------------------------------------------------
// Mach-O
// ---------------------------------------------------------------------------------------

fn uleb(out: &mut Vec<u8>, mut value: u64) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            out.push(byte);
            return;
        }
        out.push(byte | 0x80);
    }
}

fn sleb(out: &mut Vec<u8>, mut value: i64) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        let done = (value == 0 && byte & 0x40 == 0) || (value == -1 && byte & 0x40 != 0);
        out.push(if done { byte } else { byte | 0x80 });
        if done {
            return;
        }
    }
}

fn uleb_len(value: u64) -> usize {
    let mut v = Vec::new();
    uleb(&mut v, value);
    v.len()
}

fn round_up(value: u64, to: u64) -> u64 {
    value.div_ceil(to) * to
}

/// The export trie: a prefix tree over the symbol names, each terminal carrying the
/// symbol's offset in the image.
fn export_trie(symbols: &BTreeMap<String, u64>) -> Vec<u8> {
    #[derive(Default)]
    struct Node {
        terminal: Option<u64>,
        children: Vec<(String, usize)>,
        offset: usize,
    }
    let mut nodes = vec![Node::default()];
    for (name, &address) in symbols {
        let mut node = 0;
        let mut rest = name.as_str();
        loop {
            if rest.is_empty() {
                nodes[node].terminal = Some(address);
                break;
            }
            // Follow, or split, the edge sharing a prefix with what is left of the name.
            let found = nodes[node]
                .children
                .iter()
                .position(|(edge, _)| edge.as_bytes()[0] == rest.as_bytes()[0]);
            match found {
                None => {
                    nodes.push(Node {
                        terminal: Some(address),
                        ..Node::default()
                    });
                    let child = nodes.len() - 1;
                    nodes[node].children.push((rest.to_owned(), child));
                    break;
                }
                Some(i) => {
                    let (edge, child) = nodes[node].children[i].clone();
                    let common = edge
                        .bytes()
                        .zip(rest.bytes())
                        .take_while(|(a, b)| a == b)
                        .count();
                    if common == edge.len() {
                        node = child;
                    } else {
                        nodes.push(Node {
                            children: vec![(edge[common..].to_owned(), child)],
                            ..Node::default()
                        });
                        let middle = nodes.len() - 1;
                        nodes[node].children[i] = (edge[..common].to_owned(), middle);
                        node = middle;
                    }
                    rest = &rest[common..];
                }
            }
        }
    }
    // Offsets are ULEB-encoded, so sizes depend on offsets: iterate until they settle.
    let size = |node: &Node, nodes: &[Node]| -> usize {
        let terminal = node.terminal.map_or(0, |a| uleb_len(0) + uleb_len(a));
        uleb_len(terminal as u64)
            + terminal
            + 1
            + node
                .children
                .iter()
                .map(|(edge, child)| edge.len() + 1 + uleb_len(nodes[*child].offset as u64))
                .sum::<usize>()
    };
    loop {
        let mut offset = 0;
        let mut changed = false;
        for i in 0..nodes.len() {
            if nodes[i].offset != offset {
                nodes[i].offset = offset;
                changed = true;
            }
            offset += size(&nodes[i], &nodes);
        }
        if !changed {
            break;
        }
    }
    let mut out = Vec::new();
    for node in &nodes {
        match node.terminal {
            Some(address) => {
                uleb(&mut out, (uleb_len(0) + uleb_len(address)) as u64);
                uleb(&mut out, 0); // flags: a regular symbol
                uleb(&mut out, address);
            }
            None => out.push(0),
        }
        out.push(node.children.len() as u8);
        for (edge, child) in &node.children {
            out.extend_from_slice(edge.as_bytes());
            out.push(0);
            uleb(&mut out, nodes[*child].offset as u64);
        }
    }
    out
}

struct Commands {
    bytes: Vec<u8>,
    count: u32,
}

impl Commands {
    fn add(&mut self, command: u32, body: &[u8]) {
        let size = round_up(8 + body.len() as u64, 8) as usize;
        self.bytes.extend_from_slice(&command.to_le_bytes());
        self.bytes.extend_from_slice(&(size as u32).to_le_bytes());
        self.bytes.extend_from_slice(body);
        self.bytes
            .resize(self.bytes.len() + size - 8 - body.len(), 0);
        self.count += 1;
    }

    // One parameter per field of the load command.
    #[allow(clippy::too_many_arguments)]
    fn segment(
        &mut self,
        name: &str,
        vmaddr: u64,
        vmsize: u64,
        fileoff: u64,
        filesize: u64,
        prot: u32,
        sections: &[Section],
    ) {
        let mut body = Vec::new();
        body.extend_from_slice(&name16(name));
        for v in [vmaddr, vmsize, fileoff, filesize] {
            body.extend_from_slice(&v.to_le_bytes());
        }
        for v in [prot, prot, sections.len() as u32, 0] {
            body.extend_from_slice(&v.to_le_bytes());
        }
        for s in sections {
            body.extend_from_slice(&name16(s.name));
            body.extend_from_slice(&name16(name));
            body.extend_from_slice(&s.addr.to_le_bytes());
            body.extend_from_slice(&s.size.to_le_bytes());
            for v in [s.offset, s.align, 0, 0, s.flags, 0, 0, 0] {
                body.extend_from_slice(&v.to_le_bytes());
            }
        }
        self.add(0x19, &body); // LC_SEGMENT_64
    }

    fn dylib(&mut self, command: u32, name: &str) {
        let mut body = Vec::new();
        for v in [24u32, 2, 0x0001_0000, 0x0001_0000] {
            body.extend_from_slice(&v.to_le_bytes()); // name offset, timestamp, versions
        }
        body.extend_from_slice(name.as_bytes());
        body.push(0);
        self.add(command, &body);
    }
}

struct Section {
    name: &'static str,
    addr: u64,
    size: u64,
    offset: u32,
    align: u32,
    flags: u32,
}

fn name16(name: &str) -> [u8; 16] {
    let mut out = [0u8; 16];
    out[..name.len()].copy_from_slice(name.as_bytes());
    out
}

const VM_PROT_READ: u32 = 1;
const VM_PROT_WRITE: u32 = 2;
const VM_PROT_EXECUTE: u32 = 4;

/// Libraries that are part of Android itself. Their functions come from the bionic
/// compatibility library.
fn is_system_library(name: &str) -> bool {
    matches!(
        name,
        "libc.so"
            | "libm.so"
            | "libdl.so"
            | "liblog.so"
            | "libandroid.so"
            | "libjnigraphics.so"
            | "libz.so"
            | "libEGL.so"
            | "libGLESv1_CM.so"
            | "libGLESv2.so"
            | "libGLESv3.so"
            | "libOpenSLES.so"
            | "libaaudio.so"
            | "libmediandk.so"
            | "libnativewindow.so"
            | "libvulkan.so"
            | "libstdc++.so"
            | "libcamera2ndk.so"
            | "libOpenMAXAL.so"
            | "libbinder_ndk.so"
            | "libamidi.so"
            | "libsync.so"
    )
}

pub fn convert(data: &[u8], options: &Options) -> Result<Converted> {
    let elf = Elf::parse(data)?;
    if elf.has_init {
        bail!("the library has a DT_INIT function, which is not supported yet");
    }

    // --- Layout ------------------------------------------------------------------------
    let writable: Vec<Load> = elf
        .loads
        .iter()
        .copied()
        .filter(|l| l.flags & PF_W != 0)
        .collect();
    let readonly: Vec<Load> = elf
        .loads
        .iter()
        .copied()
        .filter(|l| l.flags & PF_W == 0)
        .collect();
    ensure!(!readonly.is_empty(), "no code segment");
    let text_end = readonly.iter().map(|l| l.vaddr + l.memsz).max().unwrap();
    let image_end = elf.loads.iter().map(|l| l.vaddr + l.memsz).max().unwrap();
    let data_start = match writable.iter().map(|l| l.vaddr).min() {
        Some(start) => start / PAGE * PAGE,
        None => round_up(text_end, PAGE),
    };
    ensure!(
        round_up(text_end, PAGE) <= data_start,
        "code and writable data share a page"
    );
    let data_file_end = writable
        .iter()
        .map(|l| l.vaddr + l.filesz)
        .max()
        .unwrap_or(data_start);

    let text_vmsize = SHIFT + data_start;
    let data_vmaddr = SHIFT + data_start;
    let data_vmsize = round_up(image_end.max(data_start + 1) - data_start, PAGE);
    let data_filesize = round_up(data_file_end - data_start, PAGE).min(data_vmsize);
    let stubs_vmaddr = data_vmaddr + data_vmsize;

    // The image as it will be in memory, up to the end of file-backed data.
    let mut image = vec![0u8; (data_vmaddr + data_filesize) as usize];
    for load in &elf.loads {
        let from = data
            .get(load.offset as usize..(load.offset + load.filesz) as usize)
            .context("segment extends past the end of the file")?;
        let to = (SHIFT + load.vaddr) as usize;
        image[to..to + from.len()].copy_from_slice(from);
    }

    // --- Thread pointer reads ----------------------------------------------------------
    // `mrs xN, tpidr_el0` becomes a branch to a stub that loads the address of the
    // thread slot block (in its own segment, after the stubs) into xN and branches back.
    let mut sites = Vec::new();
    for load in readonly.iter().filter(|l| l.flags & PF_X != 0) {
        let start = (SHIFT + load.vaddr) as usize;
        let end = start + load.filesz as usize;
        let mut at = start + (4 - start % 4) % 4;
        while at + 4 <= end {
            let word = u32::from_le_bytes(image[at..at + 4].try_into()?);
            if word & 0xffff_ffe0 == 0xd53b_d040 {
                sites.push((at as u64, word & 0x1f));
            }
            at += 4;
        }
    }
    let stubs_size = round_up((sites.len() as u64 * 12).max(1), PAGE);
    let slots_vmaddr = stubs_vmaddr + stubs_size;
    let mut stubs = vec![0u8; stubs_size as usize];
    for (i, &(site, register)) in sites.iter().enumerate() {
        let stub = stubs_vmaddr + i as u64 * 12;
        let branch = |from: u64, to: u64| -> Result<u32> {
            let delta = to as i64 - from as i64;
            ensure!(
                delta.abs() < (1 << 27),
                "library too large for the thread pointer stubs"
            );
            Ok(0x1400_0000 | ((delta >> 2) as u32 & 0x03ff_ffff))
        };
        let pages = ((slots_vmaddr >> 12) as i64 - (stub >> 12) as i64) as u32;
        let adrp = 0x9000_0000 | (pages & 3) << 29 | (pages >> 2 & 0x7_ffff) << 5 | register;
        let add = 0x9100_0000 | ((slots_vmaddr & 0xfff) as u32) << 10 | register << 5 | register;
        let o = i * 12;
        stubs[o..o + 4].copy_from_slice(&adrp.to_le_bytes());
        stubs[o + 4..o + 8].copy_from_slice(&add.to_le_bytes());
        stubs[o + 8..o + 12].copy_from_slice(&branch(stub + 8, site + 4)?.to_le_bytes());
        image[site as usize..site as usize + 4].copy_from_slice(&branch(site, stub)?.to_le_bytes());
    }
    // The slot block: only the stack guard is ever read from it. (One block for all
    // threads; the guard is the same for every thread on Android too.)
    let mut slots = vec![0u8; PAGE as usize];
    let guard: u64 = 0x5a3c_91e7_04b8_d200; // low byte zero, as libc's guards are
    slots[STACK_GUARD_SLOT_OFFSET..STACK_GUARD_SLOT_OFFSET + 8]
        .copy_from_slice(&guard.to_le_bytes());

    // --- Relocations ---------------------------------------------------------------------
    let needed_siblings: Vec<&String> = elf
        .needed
        .iter()
        .filter(|n| !is_system_library(n) && options.siblings.contains_key(*n))
        .collect();
    if let Some(name) = elf
        .needed
        .iter()
        .find(|n| !is_system_library(n) && !options.siblings.contains_key(*n))
    {
        bail!("the library needs {name}, which is neither part of Android nor in the app");
    }
    const ORDINAL_BIONIC: u64 = 2;
    let mut rebases: Vec<u64> = Vec::new();
    // (address, ordinal, symbol, addend, weak)
    let mut binds: Vec<(u64, u64, String, i64, bool)> = Vec::new();
    let mut missing = BTreeSet::new();

    let in_data = |address: u64| -> Result<usize> {
        ensure!(
            address >= data_start && address + 8 <= data_file_end.max(data_start),
            "relocation at {address:#x} is outside writable, file-backed data"
        );
        Ok((SHIFT + address) as usize)
    };
    for &address in &elf.relative {
        let at = in_data(address)?;
        let value = u64::from_le_bytes(image[at..at + 8].try_into()?) + SHIFT;
        image[at..at + 8].copy_from_slice(&value.to_le_bytes());
        rebases.push(SHIFT + address);
    }
    for &(address, symbol, addend, kind) in &elf.relocations {
        let at = in_data(address)?;
        match kind {
            R_AARCH64_RELATIVE => {
                image[at..at + 8].copy_from_slice(&((SHIFT as i64 + addend) as u64).to_le_bytes());
                rebases.push(SHIFT + address);
            }
            R_AARCH64_ABS64 | R_AARCH64_GLOB_DAT | R_AARCH64_JUMP_SLOT => {
                let symbol = elf
                    .symbols
                    .get(symbol as usize)
                    .context("bad symbol index")?;
                if symbol.defined {
                    let value = (SHIFT + symbol.value) as i64 + addend;
                    image[at..at + 8].copy_from_slice(&(value as u64).to_le_bytes());
                    rebases.push(SHIFT + address);
                    continue;
                }
                image[at..at + 8].fill(0);
                let weak = symbol.binding == STB_WEAK;
                let sibling = needed_siblings
                    .iter()
                    .position(|n| options.siblings[*n].contains(&symbol.name));
                let (ordinal, name) = match sibling {
                    Some(i) => (3 + i as u64, format!("_{}", symbol.name)),
                    None => {
                        let provided = options
                            .bionic_exports
                            .is_none_or(|e| e.contains(&symbol.name));
                        if provided {
                            (ORDINAL_BIONIC, format!("_bionic_{}", symbol.name))
                        } else if weak {
                            // An optional import that Android may lack as well: stays null.
                            (ORDINAL_BIONIC, format!("_bionic_{}", symbol.name))
                        } else {
                            missing.insert(symbol.name.clone());
                            (ORDINAL_BIONIC, "_bionic_unimplemented".to_owned())
                        }
                    }
                };
                binds.push((SHIFT + address, ordinal, name, addend, weak));
            }
            other => bail!("unsupported relocation type {other}"),
        }
    }
    rebases.sort_unstable();
    rebases.dedup();

    // --- Link-edit data ------------------------------------------------------------------
    const DATA_SEGMENT: u8 = 1;
    let mut rebase_info = Vec::new();
    if !rebases.is_empty() {
        rebase_info.push(0x11); // REBASE_OPCODE_SET_TYPE_IMM | pointer
        let mut i = 0;
        while i < rebases.len() {
            let mut run = 1;
            while i + run < rebases.len() && rebases[i + run] == rebases[i] + 8 * run as u64 {
                run += 1;
            }
            rebase_info.push(0x20 | DATA_SEGMENT); // SET_SEGMENT_AND_OFFSET_ULEB
            uleb(&mut rebase_info, rebases[i] - data_vmaddr);
            rebase_info.push(0x60); // DO_REBASE_ULEB_TIMES
            uleb(&mut rebase_info, run as u64);
            i += run;
        }
        rebase_info.push(0); // DONE
    }
    let mut bind_info = Vec::new();
    for (address, ordinal, name, addend, weak) in &binds {
        if *ordinal < 16 {
            bind_info.push(0x10 | *ordinal as u8); // SET_DYLIB_ORDINAL_IMM
        } else {
            bind_info.push(0x20); // SET_DYLIB_ORDINAL_ULEB
            uleb(&mut bind_info, *ordinal);
        }
        bind_info.push(0x40 | u8::from(*weak)); // SET_SYMBOL_TRAILING_FLAGS_IMM (weak import)
        bind_info.extend_from_slice(name.as_bytes());
        bind_info.push(0);
        bind_info.push(0x51); // SET_TYPE_IMM | pointer
        bind_info.push(0x60); // SET_ADDEND_SLEB
        sleb(&mut bind_info, *addend);
        bind_info.push(0x70 | DATA_SEGMENT); // SET_SEGMENT_AND_OFFSET_ULEB
        uleb(&mut bind_info, address - data_vmaddr);
        bind_info.push(0x90); // DO_BIND
    }
    if !bind_info.is_empty() {
        bind_info.push(0); // DONE
    }

    let mut exports: BTreeMap<String, u64> = elf
        .symbols
        .iter()
        .filter(|s| s.defined && s.binding != STB_LOCAL && !s.name.is_empty())
        .map(|s| (format!("_{}", s.name), SHIFT + s.value))
        .collect();
    // How the runtime recognises a converted library and finds its extent.
    exports.insert("_a2i_android_image_start".into(), 0);
    exports.insert("_a2i_android_image_end".into(), slots_vmaddr + PAGE);
    let export_info = export_trie(&exports);

    let mut linkedit = Vec::new();
    fn place(bytes: &[u8], linkedit: &mut Vec<u8>) -> (u32, u32) {
        while !linkedit.len().is_multiple_of(8) {
            linkedit.push(0);
        }
        let offset = linkedit.len() as u32;
        linkedit.extend_from_slice(bytes);
        (offset, bytes.len() as u32)
    }
    let rebase_at = place(&rebase_info, &mut linkedit);
    let bind_at = place(&bind_info, &mut linkedit);
    let export_at = place(&export_info, &mut linkedit);
    let strings_at = place(&[0u8; 8], &mut linkedit); // an empty string table
    while linkedit.len() % 16 != 0 {
        linkedit.push(0);
    }

    // --- File layout and load commands ---------------------------------------------------
    let stubs_fileoff = data_vmaddr + data_filesize;
    let slots_fileoff = stubs_fileoff + stubs_size;
    let linkedit_fileoff = slots_fileoff + PAGE;
    let linkedit_vmaddr = slots_vmaddr + PAGE;

    let mut commands = Commands {
        bytes: Vec::new(),
        count: 0,
    };
    commands.segment(
        "__TEXT",
        0,
        text_vmsize,
        0,
        text_vmsize,
        VM_PROT_READ | VM_PROT_EXECUTE,
        &[],
    );
    let mut data_sections = Vec::new();
    if let Some((address, size)) = elf.init_array {
        ensure!(
            address >= data_start && address + size <= data_file_end,
            "initializer array outside data"
        );
        data_sections.push(Section {
            name: "__mod_init_func",
            addr: SHIFT + address,
            size,
            offset: (SHIFT + address) as u32,
            align: 3,
            flags: 0x9, // S_MOD_INIT_FUNC_POINTERS
        });
    }
    commands.segment(
        "__DATA",
        data_vmaddr,
        data_vmsize,
        data_vmaddr,
        data_filesize,
        VM_PROT_READ | VM_PROT_WRITE,
        &data_sections,
    );
    commands.segment(
        "__A2I_STUBS",
        stubs_vmaddr,
        stubs_size,
        stubs_fileoff,
        stubs_size,
        VM_PROT_READ | VM_PROT_EXECUTE,
        &[],
    );
    commands.segment(
        "__A2I_SLOTS",
        slots_vmaddr,
        PAGE,
        slots_fileoff,
        PAGE,
        VM_PROT_READ | VM_PROT_WRITE,
        &[],
    );
    commands.segment(
        "__LINKEDIT",
        linkedit_vmaddr,
        round_up(linkedit.len() as u64, PAGE),
        linkedit_fileoff,
        linkedit.len() as u64,
        VM_PROT_READ,
        &[],
    );
    commands.dylib(0xd, options.install_name); // LC_ID_DYLIB
    {
        let base = linkedit_fileoff as u32;
        let at = |(offset, size): (u32, u32)| {
            if size == 0 {
                [0, 0]
            } else {
                [base + offset, size]
            }
        };
        let mut body = Vec::new();
        for pair in [at(rebase_at), at(bind_at), [0, 0], [0, 0], at(export_at)] {
            body.extend_from_slice(&pair[0].to_le_bytes());
            body.extend_from_slice(&pair[1].to_le_bytes());
        }
        commands.add(0x8000_0022, &body); // LC_DYLD_INFO_ONLY

        let mut symtab = Vec::new();
        for v in [base + strings_at.0, 0, base + strings_at.0, strings_at.1] {
            symtab.extend_from_slice(&v.to_le_bytes()); // symoff, nsyms, stroff, strsize
        }
        commands.add(0x2, &symtab); // LC_SYMTAB
        commands.add(0xb, &[0u8; 72]); // LC_DYSYMTAB
    }
    {
        // A stable identity derived from the input.
        let mut uuid = [0u8; 16];
        let mut hash = [0xcbf2_9ce4_8422_2325u64, 0x6c62_272e_07bb_0142];
        for (i, &byte) in data.iter().enumerate() {
            let h = &mut hash[i & 1];
            *h = (*h ^ byte as u64).wrapping_mul(0x0000_0100_0000_01b3);
        }
        uuid[..8].copy_from_slice(&hash[0].to_le_bytes());
        uuid[8..].copy_from_slice(&hash[1].to_le_bytes());
        uuid[6] = uuid[6] & 0x0f | 0x40;
        uuid[8] = uuid[8] & 0x3f | 0x80;
        commands.add(0x1b, &uuid); // LC_UUID
    }
    {
        let platform: u32 = match options.platform {
            Platform::Ios => 2,
            Platform::IosSimulator => 7,
        };
        let version = 17 << 16 | 4 << 8; // 17.4, the runtime's minimum
        let mut body = Vec::new();
        for v in [platform, version, version, 0] {
            body.extend_from_slice(&v.to_le_bytes());
        }
        commands.add(0x32, &body); // LC_BUILD_VERSION
    }
    commands.dylib(0xc, "/usr/lib/libSystem.B.dylib"); // ordinal 1
    commands.dylib(0xc, options.bionic); // ordinal 2
    for name in &needed_siblings {
        commands.dylib(0xc, &sibling_install_name(name)); // ordinals 3...
    }
    ensure!(
        32 + commands.bytes.len() <= SHIFT as usize,
        "too many load commands for the header page"
    );

    let mut header = Vec::new();
    for v in [
        0xfeed_facfu32, // MH_MAGIC_64
        0x0100_000c,    // CPU_TYPE_ARM64
        0,              // CPU_SUBTYPE_ARM64_ALL
        6,              // MH_DYLIB
        commands.count,
        commands.bytes.len() as u32,
        0x0010_0085, // NOUNDEFS | DYLDLINK | TWOLEVEL | NO_REEXPORTED_DYLIBS
        0,
    ] {
        header.extend_from_slice(&v.to_le_bytes());
    }
    header.extend_from_slice(&commands.bytes);
    image[..header.len()].copy_from_slice(&header);

    let mut macho = image;
    macho.extend_from_slice(&stubs);
    macho.extend_from_slice(&slots);
    macho.extend_from_slice(&linkedit);
    Ok(Converted {
        macho,
        missing: missing.into_iter().collect(),
        thread_pointer_sites: sites.len(),
    })
}

/// The install name a converted sibling library gets: `libfoo.so` → `@rpath/libfoo.dylib`.
pub fn sibling_install_name(elf_name: &str) -> String {
    format!("@rpath/{}", dylib_name(elf_name))
}

/// `libfoo.so` → `libfoo.dylib`.
pub fn dylib_name(elf_name: &str) -> String {
    format!("{}.dylib", elf_name.strip_suffix(".so").unwrap_or(elf_name))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn leb() {
        let mut v = Vec::new();
        uleb(&mut v, 624485);
        assert_eq!(v, [0xe5, 0x8e, 0x26]);
        let mut v = Vec::new();
        sleb(&mut v, -123456);
        assert_eq!(v, [0xc0, 0xbb, 0x78]);
    }

    #[test]
    fn trie_contains_all_symbols() {
        let mut symbols = BTreeMap::new();
        for (i, name) in ["_a", "_ab", "_abc", "_b", "_JNI_OnLoad", "_Java_x_y"]
            .iter()
            .enumerate()
        {
            symbols.insert(name.to_string(), 0x1000 + i as u64);
        }
        let trie = export_trie(&symbols);
        // Walk the trie for each name.
        for (name, &address) in &symbols {
            let mut at = 0usize;
            let mut rest = name.as_str();
            loop {
                let (terminal, n) = read_uleb(&trie[at..]);
                let children_at = at + n + terminal as usize;
                if rest.is_empty() {
                    assert!(terminal > 0, "{name} has no terminal");
                    let (_flags, f) = read_uleb(&trie[at + n..]);
                    let (found, _) = read_uleb(&trie[at + n + f..]);
                    assert_eq!(found, address);
                    break;
                }
                let count = trie[children_at];
                let mut p = children_at + 1;
                let mut next = None;
                for _ in 0..count {
                    let end = p + trie[p..].iter().position(|&b| b == 0).unwrap();
                    let edge = std::str::from_utf8(&trie[p..end]).unwrap();
                    let (offset, n) = read_uleb(&trie[end + 1..]);
                    if rest.starts_with(edge) {
                        next = Some((offset as usize, edge.len()));
                    }
                    p = end + 1 + n;
                }
                let (offset, eaten) = next.unwrap_or_else(|| panic!("{name} not found"));
                at = offset;
                rest = &rest[eaten..];
            }
        }
    }

    fn read_uleb(bytes: &[u8]) -> (u64, usize) {
        let mut value = 0u64;
        for (i, &b) in bytes.iter().enumerate() {
            value |= ((b & 0x7f) as u64) << (7 * i);
            if b & 0x80 == 0 {
                return (value, i + 1);
            }
        }
        unreachable!()
    }
}
