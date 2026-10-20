//! Shared validation and transaction logic for the Pocket Sync WASI plugin and importer.
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs,
    io::Read,
    path::{Path, PathBuf},
};
pub type Carts = BTreeMap<String, Vec<u8>>;
pub const MAX_CART: usize = 2 * 1024 * 1024;
pub const MAX_PACK: usize = 64 * 1024 * 1024;
pub const PACK_PATH: &str = "Assets/pico8/common/cards/bbs.p8pack";
pub const CART_PATH: &str = "Assets/pico8/common/cards/bbs";

pub fn validate_id(id: &str) -> Result<(), String> {
    if id.is_empty()
        || id.len() > 47
        || !id
            .bytes()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'_' || c == b'-')
    {
        return Err(format!("Invalid cartridge ID: {id}"));
    }
    Ok(())
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Catalog {
    version: u32,
    carts: Vec<Cart>,
}
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Cart {
    id: String,
    name: String,
    collection: String,
}
pub fn catalog() -> Result<Vec<(String, String, String)>, String> {
    let c: Catalog =
        serde_json::from_str(include_str!("../../../compatibility/offline-carts.json"))
            .map_err(|e| e.to_string())?;
    if c.version != 1 || c.carts.is_empty() || c.carts.len() > 256 {
        return Err("Unsupported or empty catalog".into());
    }
    let mut seen = std::collections::BTreeSet::new();
    for cart in &c.carts {
        validate_id(&cart.id)?;
        if !seen.insert(&cart.id)
            || cart.name.is_empty()
            || !["arcade", "sorcerer"].contains(&cart.collection.as_str())
        {
            return Err("Invalid catalog entry".into());
        }
    }
    Ok(c.carts
        .into_iter()
        .map(|x| (x.id, x.name, x.collection))
        .collect())
}
/// Fully checks PNG chunks, CRCs, dimensions and DEFLATE stream before caching.
pub fn validate_png(data: &[u8]) -> Result<(), String> {
    if data.len() > MAX_CART || !data.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Err("Expected a cartridge PNG (maximum 2 MiB)".into());
    }
    let mut pos = 8;
    let mut first = true;
    let mut ended = false;
    while pos < data.len() {
        if data.len() - pos < 12 {
            return Err("Truncated PNG chunk".into());
        }
        let n = u32::from_be_bytes(data[pos..pos + 4].try_into().unwrap()) as usize;
        let end = pos
            .checked_add(n)
            .and_then(|x| x.checked_add(12))
            .filter(|x| *x <= data.len())
            .ok_or("Truncated PNG data")?;
        let kind = &data[pos + 4..pos + 8];
        if first && (kind != b"IHDR" || n != 13) {
            return Err("Missing PNG header".into());
        }
        first = false;
        let crc = u32::from_be_bytes(data[end - 4..end].try_into().unwrap());
        if crc32fast::hash(&data[pos + 4..end - 4]) != crc {
            return Err("PNG chunk CRC mismatch".into());
        }
        if kind == b"IEND" {
            if n != 0 || end != data.len() {
                return Err("Invalid PNG end".into());
            }
            ended = true;
        }
        pos = end;
    }
    if !ended {
        return Err("Missing PNG end".into());
    }
    let mut decoder = png::Decoder::new(data);
    decoder.set_limits(png::Limits { bytes: 1024 * 1024 });
    let mut reader = decoder.read_info().map_err(|e| format!("PNG: {e}"))?;
    let info = reader.info();
    if info.width != 160
        || info.height != 205
        || info.color_type != png::ColorType::Rgba
        || info.bit_depth != png::BitDepth::Eight
        || info.interlaced
        || info.animation_control.is_some()
    {
        return Err("Expected a nonanimated 160x205 RGBA8 PICO-8 cartridge PNG".into());
    }
    let mut pixels = vec![0; reader.output_buffer_size()];
    reader
        .next_frame(&mut pixels)
        .map_err(|e| format!("PNG pixels: {e}"))?;
    reader.finish().map_err(|e| format!("PNG end: {e}"))?;
    Ok(())
}
fn le(data: &[u8], p: usize) -> u32 {
    u32::from_le_bytes(data[p..p + 4].try_into().unwrap())
}
pub fn encode_pack(carts: &Carts) -> Result<Vec<u8>, String> {
    if carts.is_empty() || carts.len() > 256 {
        return Err("Bundle needs 1..256 cartridges".into());
    }
    let size = 16 + 64 * carts.len() + carts.values().map(Vec::len).sum::<usize>();
    if size > MAX_PACK {
        return Err("Bundle exceeds 64 MiB".into());
    }
    let mut out = vec![0; 16 + 64 * carts.len()];
    out[..8].copy_from_slice(b"P8PBBS1\0");
    out[8..12].copy_from_slice(&(carts.len() as u32).to_le_bytes());
    for (i, (id, data)) in carts.iter().enumerate() {
        validate_id(id)?;
        validate_png(data)?;
        let p = 16 + 64 * i;
        out[p..p + id.len()].copy_from_slice(id.as_bytes());
        let offset = out.len() as u32;
        for (index, value) in [
            (48, offset),
            (52, data.len() as u32),
            (56, crc32fast::hash(data)),
        ] {
            out[p + index..p + index + 4].copy_from_slice(&value.to_le_bytes());
        }
        out.extend_from_slice(data);
    }
    Ok(out)
}
pub fn decode_pack(data: &[u8]) -> Result<Carts, String> {
    if data.len() < 16 || data.len() > MAX_PACK || &data[..8] != b"P8PBBS1\0" || le(data, 12) != 0 {
        return Err("Invalid bundle header".into());
    }
    let count = le(data, 8) as usize;
    if count > 256 || 16 + 64 * count > data.len() {
        return Err("Invalid bundle table".into());
    }
    let mut expected = 16 + 64 * count;
    let mut carts = Carts::new();
    for i in 0..count {
        let p = 16 + 64 * i;
        let raw = &data[p..p + 48];
        let term = raw
            .iter()
            .position(|x| *x == 0)
            .ok_or("Unterminated bundle ID")?;
        if raw[term..].iter().any(|x| *x != 0) || le(data, p + 60) != 0 {
            return Err("Nonzero bundle reserved bytes".into());
        }
        let id = std::str::from_utf8(&raw[..term]).map_err(|_| "Non-ASCII ID")?;
        validate_id(id)?;
        let offset = le(data, p + 48) as usize;
        let size = le(data, p + 52) as usize;
        if offset != expected
            || size == 0
            || size > MAX_CART
            || offset.checked_add(size).is_none_or(|end| end > data.len())
        {
            return Err("Invalid bundle payload range".into());
        }
        let payload = &data[offset..offset + size];
        if crc32fast::hash(payload) != le(data, p + 56) {
            return Err(format!("Bundle CRC mismatch: {id}"));
        }
        validate_png(payload)?;
        if carts.insert(id.into(), payload.to_vec()).is_some() {
            return Err("Duplicate bundle ID".into());
        }
        expected += size;
    }
    if expected != data.len() {
        return Err("Trailing bundle data".into());
    }
    Ok(carts)
}

#[cfg(target_arch = "wasm32")]
mod plugin;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub enum PluginMessage {
    Choice {
        name: String,
        query: String,
        choices: Vec<String>,
    },
    Text {
        name: String,
        query: String,
    },
    Exit,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
pub enum HostMessage {
    Answer { name: String, value: String },
    Kill,
}

pub fn read_bounded(path: &Path, limit: usize) -> Result<Vec<u8>, String> {
    let file = fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if file.metadata().map_err(|e| e.to_string())?.len() > limit as u64 {
        return Err(format!("File too large: {}", path.display()));
    }
    let mut data = Vec::new();
    file.take(limit as u64 + 1)
        .read_to_end(&mut data)
        .map_err(|e| e.to_string())?;
    if data.len() > limit {
        return Err("File grew beyond limit".into());
    }
    Ok(data)
}
/// Same-directory replacement: interruption before rename leaves the old file intact.
pub fn atomic_write(path: &Path, data: &[u8]) -> Result<(), String> {
    use std::io::Write;
    let parent = path.parent().ok_or("Missing parent directory")?;
    fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let temporary = path.with_extension("pocket-sync-tmp");
    let result = (|| {
        let mut f = fs::File::create(&temporary)?;
        f.write_all(data)?;
        f.sync_all()?;
        fs::rename(&temporary, path)
    })()
    .map_err(|e: std::io::Error| e.to_string());
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}
#[derive(Clone, Serialize, Deserialize)]
struct Job {
    ids: Vec<String>,
    cursor: usize,
    install_cursor: usize,
    refresh: bool,
    failures: Vec<String>,
}
#[derive(Default, Serialize, Deserialize)]
pub struct Engine {
    job: Option<Job>,
}
impl Engine {
    pub fn menu(&self, query: impl Into<String>) -> PluginMessage {
        PluginMessage::Choice {
            name: "main-menu".into(),
            query: query.into(),
            choices: [
                "Install both collections",
                "Install Arcade",
                "Install Sorcerer",
                "Update both collections",
                "Update Arcade",
                "Update Sorcerer",
                "Installed / cache status",
                "Exit",
            ]
            .iter()
            .map(|x| (*x).into())
            .collect(),
        }
    }
    pub fn status(&self, pocket: &Path, host: &Path) -> Result<String, String> {
        let mut installed = Carts::new();
        let path = pocket.join(PACK_PATH);
        let pack_status = if path.exists() {
            match read_bounded(&path, MAX_PACK).and_then(|b| decode_pack(&b)) {
                Ok(c) => {
                    installed = c;
                    String::new()
                }
                Err(e) => format!(" Existing bundle invalid: {e}."),
            }
        } else {
            String::new()
        };
        let mut lines = Vec::new();
        for collection in ["arcade", "sorcerer"] {
            let c = catalog()?
                .into_iter()
                .filter(|x| x.2 == collection)
                .collect::<Vec<_>>();
            let cached = c.iter().filter(|x| cached_cart(host, &x.0).is_ok()).count();
            let present = c.iter().filter(|x| installed.contains_key(&x.0)).count();
            lines.push(format!(
                "{collection}: installed {present}/{}, verified cache {cached}/{}",
                c.len(),
                c.len()
            ));
        }
        Ok(format!(
            "{}.{} Install reuses verified cache; Update downloads fresh copies.",
            lines.join("; "),
            pack_status
        ))
    }
    pub fn respond<F>(
        &mut self,
        input: HostMessage,
        pocket: &Path,
        host: &Path,
        mut fetch: F,
    ) -> PluginMessage
    where
        F: FnMut(&str) -> Result<(u16, Vec<u8>), String>,
    {
        match self.respond_inner(input, pocket, host, &mut fetch) {
            Ok(message) => message,
            Err(e) => {
                self.job = None;
                self.menu(format!("Stopped: {e}. The last published bundle is preserved. Retry Install to reuse verified downloads."))
            }
        }
    }
    fn respond_inner<F>(
        &mut self,
        input: HostMessage,
        pocket: &Path,
        host: &Path,
        fetch: &mut F,
    ) -> Result<PluginMessage, String>
    where
        F: FnMut(&str) -> Result<(u16, Vec<u8>), String>,
    {
        let HostMessage::Answer { name, value } = input else {
            self.job = None;
            return Ok(PluginMessage::Exit);
        };
        if name == "main-menu" {
            if value == "Exit" {
                self.job = None;
                return Ok(PluginMessage::Exit);
            }
            if value == "Installed / cache status" {
                return Ok(self.menu(self.status(pocket, host)?));
            }
            let (refresh, collection) = match value.as_str() {
                "Install both collections" => (false, "both"),
                "Install Arcade" => (false, "arcade"),
                "Install Sorcerer" => (false, "sorcerer"),
                "Update both collections" => (true, "both"),
                "Update Arcade" => (true, "arcade"),
                "Update Sorcerer" => (true, "sorcerer"),
                _ => return Err("Unknown menu selection".into()),
            };
            let ids = catalog()?
                .into_iter()
                .filter(|x| collection == "both" || x.2 == collection)
                .map(|x| x.0)
                .collect();
            self.job = Some(Job {
                ids,
                cursor: 0,
                install_cursor: 0,
                refresh,
                failures: Vec::new(),
            });
            return Ok(progress(
                "Preparing the selected collection. Verified cache will be reused unless Update was selected.",
            ));
        }
        if name != "progress" {
            return Err("Unexpected response; restart the menu".into());
        }
        if value == "Cancel" {
            self.job = None;
            return Ok(self.menu("Cancelled. Verified cache and last published bundle retained."));
        }
        if value != "Continue" {
            return Err("Unknown progress selection".into());
        }
        let job = self.job.as_mut().ok_or("No active install")?;
        if job.cursor < job.ids.len() {
            let id = &job.ids[job.cursor];
            let cached = if job.refresh {
                None
            } else {
                cached_cart(host, id).ok()
            };
            let result = if cached.is_some() {
                Ok(())
            } else {
                fetch(id).and_then(|(status, data)| {
                    if status != 200 {
                        return Err(format!("HTTP {status}"));
                    }
                    validate_png(&data)?;
                    atomic_write(&cache_path(host, id), &data)
                })
            };
            if let Err(e) = result {
                if e.contains("HTTP 429") {
                    return Err(format!(
                        "{id}: {e}; wait for the server Retry-After interval (or several minutes), then choose Install to reuse verified cache. No automatic retry"
                    ));
                }
                job.failures.push(format!("{id}: {e}"));
            }
            job.cursor += 1;
            return Ok(progress(format!(
                "Checked {}/{}. {} failure(s). The current bundle remains available until publication.",
                job.cursor,
                job.ids.len(),
                job.failures.len()
            )));
        }
        if !job.failures.is_empty() {
            let message = format!(
                "Download failed: {}. Bundle unchanged; retry Install or Update.",
                job.failures.join("; ")
            );
            self.job = None;
            return Ok(self.menu(message));
        }
        // Publish standalone carts one per response; the collection bundle is the final commit.
        if job.install_cursor < job.ids.len() {
            let id = &job.ids[job.install_cursor];
            let data = cached_cart(host, id)?;
            atomic_write(&pocket.join(CART_PATH).join(format!("{id}.p8.png")), &data)?;
            job.install_cursor += 1;
            return Ok(progress(format!(
                "Prepared standalone carts {}/{}. Publishing the collection bundle after all selected carts are ready.",
                job.install_cursor,
                job.ids.len()
            )));
        }
        let path = pocket.join(PACK_PATH);
        let mut carts = if path.exists() {
            decode_pack(&read_bounded(&path, MAX_PACK)?)?
        } else {
            Carts::new()
        };
        for id in &job.ids {
            carts.insert(id.clone(), cached_cart(host, id)?);
        }
        let data = encode_pack(&carts)?;
        atomic_write(&path, &data)?;
        let count = job.ids.len();
        self.job = None;
        Ok(self.menu(format!(
            "Installed {count} selected cartridges; bundle contains {} total. {}",
            carts.len(),
            self.status(pocket, host)?
        )))
    }
}
fn progress(query: impl Into<String>) -> PluginMessage {
    PluginMessage::Choice {
        name: "progress".into(),
        query: query.into(),
        choices: vec!["Continue".into(), "Cancel".into()],
    }
}
pub fn cache_path(host: &Path, id: &str) -> PathBuf {
    host.join("pico8-offline-cache")
        .join(format!("{id}.p8.png"))
}
pub fn cached_cart(host: &Path, id: &str) -> Result<Vec<u8>, String> {
    validate_id(id)?;
    let data = read_bounded(&cache_path(host, id), MAX_CART)?;
    validate_png(&data)?;
    Ok(data)
}

/// Import only catalog IDs from an explicit cache directory; never discovers a user's home.
pub fn import_cache(source: &Path, host: &Path, collection: &str) -> Result<usize, String> {
    if !["arcade", "sorcerer", "both"].contains(&collection) {
        return Err("Collection must be arcade, sorcerer or both".into());
    }
    let mut staged = Carts::new();
    for (id, _, kind) in catalog()?
        .into_iter()
        .filter(|x| collection == "both" || x.2 == collection)
    {
        let _ = kind;
        let path = source.join(format!("{id}.p8.png"));
        let data = read_bounded(&path, MAX_CART)?;
        validate_png(&data).map_err(|e| format!("{id}: {e}"))?;
        staged.insert(id, data);
    }
    for (id, data) in &staged {
        atomic_write(&cache_path(host, id), data)?;
    }
    Ok(staged.len())
}

impl Engine {
    /// Drive the resumable transaction steps while reporting progress to the host.
    pub fn respond_automatic<F, P>(
        &mut self,
        input: HostMessage,
        pocket: &Path,
        host: &Path,
        mut fetch: F,
        mut progress: P,
    ) -> PluginMessage
    where
        F: FnMut(&str) -> Result<(u16, Vec<u8>), String>,
        P: FnMut(&str),
    {
        let mut message = self.respond(input, pocket, host, &mut fetch);
        loop {
            match &message {
                PluginMessage::Choice { name, query, .. } if name == "progress" => progress(query),
                _ => return message,
            }
            message = self.respond(
                HostMessage::Answer {
                    name: "progress".into(),
                    value: "Continue".into(),
                },
                pocket,
                host,
                &mut fetch,
            );
        }
    }
}
