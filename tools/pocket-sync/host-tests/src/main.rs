//! Real Extism/WASI harness using the pinned official demo's mounts, host ABI and enum protocol.
use extism::*;
use pocket_sync::{Carts, PACK_PATH, atomic_write, cache_path, decode_pack, encode_pack};
use serde_json::{Value, json};
use std::{
    fs,
    path::{Path, PathBuf},
    time::Instant,
};
host_fn!(pub print_msg(message:&str) {println!("plugin: {message}");Ok(())});
host_fn!(pub open_url(_url:&str) {Ok(())});
fn load(wasm: &Path, pocket: &Path, host: &Path, http: bool) -> Plugin {
    let mut manifest = Manifest::new([Wasm::file(wasm)])
        .with_allowed_path(pocket.display().to_string(), "pocket")
        .with_allowed_path(host.display().to_string(), "host");
    if http {
        manifest = manifest.with_allowed_host("www.lexaloffle.com");
    }
    PluginBuilder::new(manifest)
        .with_cache_disabled()
        .with_wasi(true)
        .with_function("print_msg", [PTR], [PTR], UserData::new(()), print_msg)
        .with_function("open_url", [PTR], [PTR], UserData::new(()), open_url)
        .build()
        .unwrap()
}
fn call(p: &mut Plugin, name: &str, input: Value) -> Value {
    let t = Instant::now();
    let out: String = if name == "start" {
        p.call::<Option<()>, String>(name, None).unwrap()
    } else {
        p.call(name, serde_json::to_string(&input).unwrap())
            .unwrap()
    };
    println!("call {name}: {}ms", t.elapsed().as_millis());
    serde_json::from_str(&out).unwrap()
}
fn answer(p: &mut Plugin, name: &str, value: &str) -> Value {
    call(
        p,
        "handle_response",
        json!({"Answer":{"name":name,"value":value}}),
    )
}
#[derive(Debug)]
struct Options {
    wasm: PathBuf,
    root: PathBuf,
    source: Option<PathBuf>,
    live: bool,
    throttle: bool,
    catalog: bool,
}

fn parse_options(args: &[String]) -> Result<Options, String> {
    if args.len() < 3 {
        return Err("usage: host-tests plugin.wasm test-root (verified-cache-dir | --fixtures) [--catalog] [--live] [--live-throttle]".into());
    }
    let source = if args[2] == "--fixtures" {
        None
    } else if args[2].starts_with("--") {
        return Err("Expected verified-cache-dir or --fixtures".into());
    } else {
        Some(PathBuf::from(&args[2]))
    };
    let mut options = Options {
        wasm: PathBuf::from(&args[0]),
        root: PathBuf::from(&args[1]),
        source,
        live: false,
        throttle: false,
        catalog: false,
    };
    for flag in &args[3..] {
        match flag.as_str() {
            "--live" => options.live = true,
            "--live-throttle" => options.throttle = true,
            "--catalog" => options.catalog = true,
            _ => return Err(format!("Unknown option: {flag}")),
        }
    }
    if options.source.is_none() && (options.live || options.throttle) {
        return Err("--fixtures cannot use live network options".into());
    }
    Ok(options)
}

fn fixture_carts() -> Result<Carts, String> {
    let mut carts = Carts::new();
    for (index, (id, _, _)) in pocket_sync::catalog()?.into_iter().enumerate() {
        let mut data = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut data, 160, 205);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            encoder.add_text_chunk(
                "Description".into(),
                format!("PROJECT-OWNED FIXTURE {id}: generated transaction test image; no game content"),
            ).map_err(|e| e.to_string())?;
            // Distinct project-owned pixels let byte-identity checks detect ID swaps.
            let pixels = [index as u8, 64, 128, 255].repeat(160 * 205);
            encoder
                .write_header()
                .map_err(|e| e.to_string())?
                .write_image_data(&pixels)
                .map_err(|e| e.to_string())?;
        }
        pocket_sync::validate_png(&data)?;
        carts.insert(id, data);
    }
    Ok(carts)
}

fn write_fixtures(root: &Path) -> PathBuf {
    let source = root.join("source-fixtures");
    fs::create_dir_all(&source).unwrap();
    for (id, bytes) in fixture_carts().unwrap() {
        atomic_write(&source.join(format!("{id}.p8.png")), &bytes).unwrap();
    }
    fs::write(source.join("README.txt"),
        "PROJECT-OWNED FIXTURES: generated 160x205 RGBA8 PNGs for installer transactions.\nThese images contain no games and establish no game compatibility.\n").unwrap();
    println!("Generated all 36 project-owned PNG fixtures; no game content or HTTP access");
    source
}

fn main() {
    let options =
        parse_options(&std::env::args().skip(1).collect::<Vec<_>>()).unwrap_or_else(|error| {
            eprintln!("{error}");
            std::process::exit(2)
        });
    let fixtures = options.source.is_none();
    let Options {
        wasm,
        root,
        source,
        live,
        throttle,
        catalog,
    } = options;
    let source = source.unwrap_or_else(|| write_fixtures(&root));
    let pocket = root.join("pocket");
    let host = root.join("host");
    fs::create_dir_all(&pocket).unwrap();
    fs::create_dir_all(&host).unwrap();
    for id in ["15133", "mot_sorcerergame"] {
        let b = fs::read(source.join(format!("{id}.p8.png"))).unwrap();
        atomic_write(&cache_path(&host, id), &b).unwrap();
    }
    let mut c = Carts::new();
    c.insert(
        "15133".into(),
        fs::read(cache_path(&host, "15133")).unwrap(),
    );
    let initial = encode_pack(&c).unwrap();
    atomic_write(&pocket.join(PACK_PATH), &initial).unwrap();
    let mut p = load(&wasm, &pocket, &host, false);
    assert_eq!(
        call(&mut p, "start", Value::Null)["Choice"]["name"],
        "main-menu"
    );
    assert_eq!(
        call(&mut p, "handle_response", json!("Kill")),
        json!("Exit")
    );
    assert_eq!(fs::read(pocket.join(PACK_PATH)).unwrap(), initial);
    call(&mut p, "start", Value::Null);
    let result = answer(&mut p, "main-menu", "Install Sorcerer");
    assert!(
        result["Choice"]["query"]
            .as_str()
            .unwrap()
            .starts_with("Installed 1"),
        "{result}"
    );
    let merged = fs::read(pocket.join(PACK_PATH)).unwrap();
    assert_eq!(decode_pack(&merged).unwrap().len(), 2);
    let old_cache = fs::read(cache_path(&host, "mot_sorcerergame")).unwrap();
    let denied = p.call::<String, String>(
        "handle_response",
        serde_json::to_string(&json!({"Answer":{"name":"main-menu","value":"Update Sorcerer"}}))
            .unwrap(),
    );
    assert!(
        denied.is_err(),
        "HTTP disallowed by manifest must trap in real Extism host"
    );
    assert_eq!(fs::read(pocket.join(PACK_PATH)).unwrap(), merged);
    assert_eq!(
        fs::read(cache_path(&host, "mot_sorcerergame")).unwrap(),
        old_cache
    );
    p = load(&wasm, &pocket, &host, false);
    call(&mut p, "start", Value::Null);
    answer(&mut p, "main-menu", "Installed / cache status");
    assert_eq!(answer(&mut p, "main-menu", "Exit"), json!("Exit"));
    if catalog {
        let imported = pocket_sync::import_cache(&source, &host, "both").unwrap();
        assert_eq!(imported, 36);
        let mut real = load(&wasm, &pocket, &host, false);
        call(&mut real, "start", Value::Null);
        let result = answer(&mut real, "main-menu", "Install both collections");
        assert!(
            result["Choice"]["query"]
                .as_str()
                .unwrap()
                .starts_with("Installed 36"),
            "{result}"
        );
        let installed = decode_pack(&fs::read(pocket.join(PACK_PATH)).unwrap()).unwrap();
        assert_eq!(installed.len(), 36);
        for (id, bytes) in installed {
            assert_eq!(
                bytes,
                fs::read(source.join(format!("{id}.p8.png"))).unwrap(),
                "original bytes changed for {id}"
            );
        }
        let kind = if fixtures {
            "project-owned fixtures"
        } else {
            "curated originals"
        };
        println!("All 36 {kind} installed by actual WASI plugin from cache, bytes preserved: PASS");
    }
    // Labeled transaction fixtures: this does not establish each named game's semantics.
    let fixture = fs::read(source.join("15133.p8.png")).unwrap();
    for (id, _, _) in pocket_sync::catalog().unwrap() {
        atomic_write(&cache_path(&host, &id), &fixture).unwrap();
    }
    let mut cancelled = load(&wasm, &pocket, &host, false);
    call(&mut cancelled, "start", Value::Null);
    let before_cancel = fs::read(pocket.join(PACK_PATH)).unwrap();
    let cancel = cancelled.cancel_handle();
    let thread = std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_millis(2));
        cancel.cancel().unwrap();
    });
    let interrupted = cancelled.call::<String, String>(
        "handle_response",
        serde_json::to_string(&json!({"Answer":{"name":"main-menu","value":"Install Arcade"}}))
            .unwrap(),
    );
    thread.join().unwrap();
    assert!(interrupted.is_err(), "expected actual Extism cancellation");
    assert_eq!(fs::read(pocket.join(PACK_PATH)).unwrap(), before_cancel);
    assert!(decode_pack(&before_cancel).is_ok());
    println!("Hard cancellation of automatic install preserved last valid pack: PASS");
    if live {
        let mut livep = load(&wasm, &pocket, &host, true);
        call(&mut livep, "start", Value::Null);
        let result = answer(&mut livep, "main-menu", "Update Sorcerer");
        assert!(
            result["Choice"]["query"]
                .as_str()
                .unwrap()
                .starts_with("Installed 1"),
            "{result}"
        );
        println!("LIVE HTTPS real WASI Sorcerer download and install: PASS");
    }
    if throttle {
        if live {
            std::thread::sleep(std::time::Duration::from_secs(15));
        }
        for id in ["62288", "64085"] {
            fs::remove_file(cache_path(&host, id)).unwrap();
        }
        let mut livep = load(&wasm, &pocket, &host, true);
        call(&mut livep, "start", Value::Null);
        let start = Instant::now();
        let result = answer(&mut livep, "main-menu", "Install Arcade");
        assert!(
            result["Choice"]["query"]
                .as_str()
                .unwrap()
                .starts_with("Installed 35"),
            "{result}"
        );
        assert!(start.elapsed() >= std::time::Duration::from_secs(15));
        println!(
            "LIVE HTTPS two-cart automatic loop and WASI poll_oneoff 15s courtesy interval: PASS"
        );
    }
    println!(
        "REAL EXTISM/WASI ABI, automatic cache install, kill, merge, denied HTTP preservation: PASS"
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generated_fixtures_validate_and_round_trip_all_catalog_entries() {
        let carts = fixture_carts().expect("generate project-owned PNG fixtures");
        assert_eq!(carts.len(), 36);
        for (id, _, _) in pocket_sync::catalog().unwrap() {
            let data = &carts[&id];
            pocket_sync::validate_png(data).unwrap();
            let label = b"PROJECT-OWNED FIXTURE";
            assert!(data.windows(label.len()).any(|part| part == label));
        }
        assert_eq!(decode_pack(&encode_pack(&carts).unwrap()).unwrap(), carts);
    }

    fn options(arguments: &[&str]) -> Result<Options, String> {
        parse_options(&arguments.iter().map(|x| x.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn fixture_invocation_needs_no_external_cache() {
        let parsed = options(&["plugin.wasm", "test-root", "--fixtures", "--catalog"]).unwrap();
        assert!(parsed.source.is_none());
        assert!(parsed.catalog);
        assert!(!parsed.live && !parsed.throttle);
    }

    #[test]
    fn fixture_invocation_rejects_live_network_options() {
        for flag in ["--live", "--live-throttle"] {
            assert!(
                options(&["plugin.wasm", "test-root", "--fixtures", flag])
                    .unwrap_err()
                    .contains("cannot use live network")
            );
        }
    }

    #[test]
    fn existing_verified_cache_invocation_is_preserved() {
        let parsed = options(&[
            "plugin.wasm",
            "test-root",
            "verified-cache",
            "--live",
            "--catalog",
        ])
        .unwrap();
        assert_eq!(parsed.source, Some(PathBuf::from("verified-cache")));
        assert!(parsed.live && parsed.catalog);
    }
}
