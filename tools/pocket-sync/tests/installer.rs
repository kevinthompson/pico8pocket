use pocket_sync::*;
use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicUsize, Ordering},
};
static NEXT: AtomicUsize = AtomicUsize::new(0);
fn dirs() -> (PathBuf, PathBuf, PathBuf) {
    let root = std::env::temp_dir().join(format!(
        "pocket-sync-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::SeqCst)
    ));
    fs::create_dir_all(root.join("pocket")).unwrap();
    fs::create_dir_all(root.join("host")).unwrap();
    (root.clone(), root.join("pocket"), root.join("host"))
}
fn cart() -> Vec<u8> {
    let mut o = Vec::new();
    {
        let mut e = png::Encoder::new(&mut o, 160, 205);
        e.set_color(png::ColorType::Rgba);
        e.set_depth(png::BitDepth::Eight);
        e.write_header()
            .unwrap()
            .write_image_data(&vec![0; 160 * 205 * 4])
            .unwrap();
    }
    o
}
fn answer(name: &str, value: &str) -> HostMessage {
    HostMessage::Answer {
        name: name.into(),
        value: value.into(),
    }
}
#[test]
fn exact_official_wire_units_and_choice() {
    assert_eq!(
        serde_json::to_string(&PluginMessage::Exit).unwrap(),
        "\"Exit\""
    );
    assert!(matches!(
        serde_json::from_str::<HostMessage>("\"Kill\"").unwrap(),
        HostMessage::Kill
    ));
    let e = Engine::default();
    assert!(serde_json::to_value(e.menu("Ready")).unwrap()["Choice"]["choices"].is_array());
}
#[test]
fn failed_update_keeps_previous_pack_and_cache() {
    let (r, p, h) = dirs();
    let mut c = Carts::new();
    c.insert("mot_sorcerergame".into(), cart());
    let old = encode_pack(&c).unwrap();
    atomic_write(&p.join(PACK_PATH), &old).unwrap();
    atomic_write(&cache_path(&h, "mot_sorcerergame"), &cart()).unwrap();
    let mut e = Engine::default();
    e.respond(answer("main-menu", "Update Sorcerer"), &p, &h, |_| panic!());
    e.respond(answer("progress", "Continue"), &p, &h, |_| {
        Ok((500, vec![]))
    });
    e.respond(answer("progress", "Continue"), &p, &h, |_| panic!());
    assert_eq!(fs::read(p.join(PACK_PATH)).unwrap(), old);
    assert!(cached_cart(&h, "mot_sorcerergame").is_ok());
    fs::remove_dir_all(r).unwrap();
}
#[test]
fn cache_reinstall_merges_preserves_arcade_and_kill_does_not_publish() {
    let (r, p, h) = dirs();
    let mut c = Carts::new();
    c.insert("15133".into(), cart());
    let old = encode_pack(&c).unwrap();
    atomic_write(&p.join(PACK_PATH), &old).unwrap();
    atomic_write(&cache_path(&h, "mot_sorcerergame"), &cart()).unwrap();
    let mut e = Engine::default();
    e.respond(
        answer("main-menu", "Install Sorcerer"),
        &p,
        &h,
        |_| panic!(),
    );
    e.respond(answer("progress", "Continue"), &p, &h, |_| {
        panic!("cache must avoid network")
    });
    assert_eq!(
        e.respond(HostMessage::Kill, &p, &h, |_| panic!()),
        PluginMessage::Exit
    );
    assert_eq!(fs::read(p.join(PACK_PATH)).unwrap(), old);
    e.respond(
        answer("main-menu", "Install Sorcerer"),
        &p,
        &h,
        |_| panic!(),
    );
    for _ in 0..3 {
        e.respond(answer("progress", "Continue"), &p, &h, |_| panic!());
    }
    let merged = decode_pack(&fs::read(p.join(PACK_PATH)).unwrap()).unwrap();
    assert_eq!(merged.len(), 2);
    assert!(p.join(CART_PATH).join("mot_sorcerergame.p8.png").exists());
    fs::remove_dir_all(r).unwrap();
}
#[test]
fn malformed_http_200_never_enters_cache() {
    let (r, p, h) = dirs();
    let mut e = Engine::default();
    e.respond(
        answer("main-menu", "Install Sorcerer"),
        &p,
        &h,
        |_| panic!(),
    );
    e.respond(answer("progress", "Continue"), &p, &h, |_| {
        Ok((200, b"<html>error</html>".to_vec()))
    });
    assert!(!cache_path(&h, "mot_sorcerergame").exists());
    assert!(!p.join(PACK_PATH).exists());
    fs::remove_dir_all(r).unwrap();
}
#[test]
fn explicit_cache_import_is_transactional_for_invalid_selection() {
    let (r, _p, h) = dirs();
    let source = r.join("source");
    fs::create_dir_all(&source).unwrap();
    fs::write(source.join("mot_sorcerergame.p8.png"), cart()).unwrap();
    assert_eq!(import_cache(&source, &h, "sorcerer").unwrap(), 1);
    assert!(cached_cart(&h, "mot_sorcerergame").is_ok());
    assert!(import_cache(&source, &h, "../arcade").is_err());
    fs::write(source.join("mot_sorcerergame.p8.png"), b"broken").unwrap();
    assert!(import_cache(&source, &h, "sorcerer").is_err());
    assert!(cached_cart(&h, "mot_sorcerergame").is_ok());
    fs::remove_dir_all(r).unwrap();
}

#[test]
fn rate_limit_stops_batch_without_fetching_next_or_clobbering_cache() {
    let (r, p, h) = dirs();
    atomic_write(&cache_path(&h, "62288"), &cart()).unwrap();
    let mut e = Engine::default();
    e.respond(answer("main-menu", "Update Arcade"), &p, &h, |_| panic!());
    let message = e.respond(answer("progress", "Continue"), &p, &h, |id| {
        assert_eq!(id, "62288");
        Ok((429, vec![]))
    });
    let PluginMessage::Choice { name, query, .. } = message else {
        panic!()
    };
    assert_eq!(name, "main-menu");
    assert!(query.contains("429"));
    assert!(query.contains("wait"));
    assert!(cached_cart(&h, "62288").is_ok());
    assert!(!p.join(PACK_PATH).exists());
    e.respond(answer("progress", "Continue"), &p, &h, |_| {
        panic!("must not retry after 429")
    });
    fs::remove_dir_all(r).unwrap();
}

#[test]
fn failed_rename_keeps_existing_destination_and_removes_temp() {
    let (r, _, _) = dirs();
    let destination = r.join("occupied");
    fs::create_dir_all(&destination).unwrap();
    fs::write(destination.join("last-valid"), b"original").unwrap();
    assert!(atomic_write(&destination, b"replacement").is_err());
    assert_eq!(
        fs::read(destination.join("last-valid")).unwrap(),
        b"original"
    );
    assert!(!destination.with_extension("pocket-sync-tmp").exists());
    fs::remove_dir_all(r).unwrap();
}

#[test]
fn automatic_install_reports_progress_and_finishes_without_repeated_questions() {
    let (r, p, h) = dirs();
    atomic_write(&cache_path(&h, "mot_sorcerergame"), &cart()).unwrap();
    let mut messages = Vec::new();
    let message = Engine::default().respond_automatic(
        answer("main-menu", "Install Sorcerer"),
        &p,
        &h,
        |_| panic!("verified cache avoids HTTP"),
        |s| messages.push(s.to_owned()),
    );
    let PluginMessage::Choice { name, query, .. } = message else {
        panic!()
    };
    assert_eq!(name, "main-menu");
    assert!(query.starts_with("Installed 1"));
    assert!(messages.iter().any(|s| s.contains("1/1")));
    assert!(p.join(PACK_PATH).exists());
    fs::remove_dir_all(r).unwrap();
}
