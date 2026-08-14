pub mod analysis;
pub mod audio_bridge;
pub mod commands;
pub mod dj_brain;
pub mod library;
pub mod models;

pub fn create_app() -> tauri::Builder<tauri::Wry> {
    tauri::Builder::default().invoke_handler(tauri::generate_handler![
        commands::get_app_version,
        commands::ping_audio_engine,
        commands::get_sample_track
    ])
}
