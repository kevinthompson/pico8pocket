use pocket_sync::*;
fn png_cart() -> Vec<u8> {
    let mut out = Vec::new();
    {
        let mut e = png::Encoder::new(&mut out, 160, 205);
        e.set_color(png::ColorType::Rgba);
        e.set_depth(png::BitDepth::Eight);
        let mut w = e.write_header().unwrap();
        w.write_image_data(&vec![0; 160 * 205 * 4]).unwrap();
    }
    out
}
#[test]
fn identifiers_are_path_safe_and_canonical() {
    for id in ["15133", "magic_bubble", "a-b", "x"] {
        assert!(validate_id(id).is_ok());
    }
    for id in [
        "",
        "#15133",
        "../cart",
        "a/b",
        "Cart",
        "a.b",
        &"a".repeat(48),
    ] {
        assert!(validate_id(id).is_err(), "{id}");
    }
}
#[test]
fn curated_catalog_has_36_unique_safe_targets() {
    let c = catalog().unwrap();
    assert_eq!(c.len(), 36);
    assert_eq!(c.iter().filter(|x| x.2 == "arcade").count(), 35);
    assert!(c.iter().any(|x| x.0 == "mot_sorcerergame"));
}
#[test]
fn validates_complete_png_and_rejects_truncation_crc_html_wrong_dimensions() {
    let p = png_cart();
    assert!(validate_png(&p).is_ok());
    for n in [0, 8, 33, p.len() - 1] {
        assert!(validate_png(&p[..n]).is_err());
    }
    let mut bad = p.clone();
    bad[40] ^= 1;
    assert!(validate_png(&bad).is_err());
    assert!(validate_png(b"<html></html>").is_err());
    let mut wrong = Vec::new();
    {
        let mut e = png::Encoder::new(&mut wrong, 159, 205);
        e.set_color(png::ColorType::Rgba);
        e.write_header()
            .unwrap()
            .write_image_data(&vec![0; 159 * 205 * 4])
            .unwrap();
    }
    assert!(validate_png(&wrong).is_err());
    assert!(validate_png(&vec![0; MAX_CART + 1]).is_err());
    let mut extra = p;
    extra.push(0);
    assert!(validate_png(&extra).is_err());
}
#[test]
fn pack_is_deterministic_and_merge_preserves_other_collection() {
    let mut c = Carts::new();
    c.insert("mot_sorcerergame".into(), png_cart());
    c.insert("15133".into(), png_cart());
    let bytes = encode_pack(&c).unwrap();
    assert_eq!(&bytes[..8], b"P8PBBS1\0");
    assert_eq!(decode_pack(&bytes).unwrap(), c);
    assert_eq!(encode_pack(&decode_pack(&bytes).unwrap()).unwrap(), bytes);
}
#[test]
fn pack_rejects_reserved_duplicate_noncontiguous_crc_and_truncation() {
    let mut c = Carts::new();
    c.insert("a".into(), png_cart());
    c.insert("b".into(), png_cart());
    let p = encode_pack(&c).unwrap();
    for pos in [0, 12, 16 + 47, 16 + 48, 16 + 56, 16 + 60] {
        let mut bad = p.clone();
        bad[pos] ^= 1;
        assert!(decode_pack(&bad).is_err(), "byte {pos}");
    }
    let mut dup = p.clone();
    dup[80] = b'a';
    assert!(decode_pack(&dup).is_err());
    assert!(decode_pack(&p[..p.len() - 1]).is_err());
    assert!(encode_pack(&Carts::new()).is_err());
}

#[test]
fn invalid_deflate_with_valid_chunk_crc_is_rejected() {
    let mut p = png_cart();
    let mut pos = 8;
    loop {
        let n = u32::from_be_bytes(p[pos..pos + 4].try_into().unwrap()) as usize;
        if &p[pos + 4..pos + 8] == b"IDAT" {
            p[pos + 8] = 0;
            let crc = crc32fast::hash(&p[pos + 4..pos + 8 + n]);
            p[pos + 8 + n..pos + 12 + n].copy_from_slice(&crc.to_be_bytes());
            break;
        }
        pos += n + 12;
    }
    assert!(validate_png(&p).is_err());
}

#[test]
fn reads_empty_version_one_bundle_for_existing_install_upgrade() {
    let mut bytes = vec![0; 16];
    bytes[..8].copy_from_slice(b"P8PBBS1\0");
    assert!(decode_pack(&bytes).unwrap().is_empty());
}
