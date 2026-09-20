use std::{path::PathBuf,fs::File,io::BufWriter};
use brawllib_rs::{brawl_mod::BrawlMod,high_level_fighter::HighLevelFighter};
fn main() {
 env_logger::init();
 let args:Vec<String>=std::env::args().collect();
 let brawl=BrawlMod::new(&PathBuf::from(&args[1]),None);
 let fighters=brawl.load_fighters(true).expect("load extracted fighters");
 let f=fighters.iter().find(|f|f.cased_name.eq_ignore_ascii_case("Diddy")).expect("Diddy assets");
 let data=f.get_fighter_data().unwrap();let vis=&data.model_visibility;
 let refs:Vec<_>=vis.references.iter().map(|r|r.bone_switches.iter().map(|s|s.groups.iter().map(|g|g.bones.clone()).collect::<Vec<_>>()).collect::<Vec<_>>()).collect();
 let defaults:Vec<_>=vis.defaults.iter().map(|d|serde_json::json!({"switch":d.switch_index,"group":d.group_index})).collect();
 let metadata=serde_json::json!({"visibility":{"references":refs,"defaults":defaults},"hurtboxes":data.misc.hurt_boxes});
 serde_json::to_writer(BufWriter::new(File::create(format!("{}.fighter.json",args[2])).unwrap()),&metadata).unwrap();
 let h=HighLevelFighter::new(f);
 println!("Diddy: {} actions, {} subactions",h.actions.len(),h.subactions.len());
 serde_json::to_writer(BufWriter::new(File::create(&args[2]).unwrap()),&h).unwrap();
}
