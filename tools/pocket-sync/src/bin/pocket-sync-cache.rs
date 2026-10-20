//! Optional native companion: explicit paths only; not part of the WASI sandbox.
use pocket_sync::*;
use std::path::Path;
fn run() -> Result<(), String> {
    let args = std::env::args().collect::<Vec<_>>();
    match args.get(1).map(String::as_str){
        Some("import") if args.len()==5 => {let count=import_cache(Path::new(&args[2]),Path::new(&args[3]),&args[4])?;println!("Imported {count} verified cartridges into {}/pico8-offline-cache",args[3]);}
        Some("install") if args.len()==5=>{
            let mut engine=Engine::default();let selection=match args[4].as_str(){"both"=>"Install both collections","arcade"=>"Install Arcade","sorcerer"=>"Install Sorcerer",_=>return Err("Unknown collection".into())};
            let pocket=Path::new(&args[3]);let host=Path::new(&args[2]);
            let mut message=engine.respond(HostMessage::Answer{name:"main-menu".into(),value:selection.into()},pocket,host,|_|Err("Not in verified cache; import the explicit source cache first".into()));
            loop {match &message{PluginMessage::Choice{name,query,..} if name=="progress"=>{println!("{query}");message=engine.respond(HostMessage::Answer{name:"progress".into(),value:"Continue".into()},pocket,host,|_|Err("Not in verified cache; import the explicit source cache first".into()));},PluginMessage::Choice{query,..}=>{println!("{query}");if !query.starts_with("Installed "){return Err("Installation did not complete".into());}break;},_=>return Err("Unexpected installer response".into())}}
        }
        Some("verify") if args.len()==3=>{let data=read_bounded(Path::new(&args[2]),MAX_PACK)?;let carts=decode_pack(&data)?;println!("Valid bundle: {} cartridges, {} bytes",carts.len(),data.len());}
        _=>return Err("Usage: pocket-sync-cache import SOURCE_CACHE HOST_DIR both|arcade|sorcerer\n       pocket-sync-cache install HOST_DIR POCKET_ROOT both|arcade|sorcerer\n       pocket-sync-cache verify FILE.p8pack".into())
    }
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        eprintln!("{e}");
        std::process::exit(1);
    }
}
