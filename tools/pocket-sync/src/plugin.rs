//! Pocket Plugin ABI, based on the official pinned template (see README).
use super::*;
use extism_pdk::*;

#[host_fn]
unsafe extern "ExtismHost" {
    fn print_msg(message: &str) -> ();
}

#[plugin_fn]
pub fn start() -> FnResult<String> {
    var::remove("pocket-sync-state")?;
    Ok(serde_json::to_string(&Engine::default().menu("Pocket Sync: offline Pico Arcade and Sorcerer collections. Install uses verified cache; Update refreshes downloads."))?)
}
#[plugin_fn]
pub fn handle_response(input: String) -> FnResult<String> {
    let mut engine: Engine = match var::get::<Vec<u8>>("pocket-sync-state")? {
        Some(data) => serde_json::from_slice(&data)?,
        None => Engine::default(),
    };
    let mut previous_request: Option<std::time::Instant> = None;
    let message = engine.respond_automatic(
        serde_json::from_str(&input)?,
        Path::new("pocket"),
        Path::new("host"),
        |id| {
            if let Some(previous) = previous_request {
                let remaining =
                    std::time::Duration::from_secs(15).saturating_sub(previous.elapsed());
                if !remaining.is_zero() {
                    unsafe {
                        let _ = print_msg(
                            "Waiting between downloads to respect Lexaloffle rate limits.\n",
                        );
                    }
                    // Short WASI polls return control to the VM so a host cancellation
                    // does not wait for the full courtesy interval.
                    while previous.elapsed() < std::time::Duration::from_secs(15) {
                        let left =
                            std::time::Duration::from_secs(15).saturating_sub(previous.elapsed());
                        std::thread::sleep(left.min(std::time::Duration::from_millis(100)));
                    }
                }
            }
            previous_request = Some(std::time::Instant::now());
            unsafe {
                let _ = print_msg(&format!("Downloading #{id}...\n"));
            }
            let url =
                format!("https://www.lexaloffle.com/bbs/get_cart.php?cat=7&play_src=2&lid={id}");
            let request = HttpRequest::new(&url).with_method("GET");
            let response = http::request::<()>(&request, None).map_err(|e| e.to_string())?;
            let status = response.status_code();
            if status == 429 {
                return Err(format!(
                    "HTTP 429; Retry-After: {}",
                    response
                        .header("retry-after")
                        .or_else(|| response.header("Retry-After"))
                        .unwrap_or("not supplied")
                ));
            }
            // Extism buffers HTTP responses. Check its length before copying into our cache.
            if response.as_memory().len() > MAX_CART {
                return Err("HTTP response exceeds 2 MiB cartridge limit".into());
            }
            Ok((status, response.body()))
        },
        |message| unsafe {
            let _ = print_msg(&format!("{message}\n"));
        },
    );
    var::set("pocket-sync-state", serde_json::to_vec(&engine)?)?;
    unsafe {
        let _ = print_msg(&format!(
            "{}\n",
            match &message {
                PluginMessage::Choice { query, .. } | PluginMessage::Text { query, .. } =>
                    query.as_str(),
                PluginMessage::Exit => "Exited",
            }
        ));
    }
    Ok(serde_json::to_string(&message)?)
}
