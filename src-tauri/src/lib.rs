pub mod analysis;
pub mod audio_bridge;
pub mod commands;
pub mod dj_brain;
pub mod library;
pub mod models;

use audio_bridge::AudioBridge;
use tauri::Manager;

pub fn create_app() -> tauri::Builder<tauri::Wry> {
    tauri::Builder::default()
        .setup(|app| {
            // The AudioBridge is the single application-level owner of the
            // C++ engine (see Docs/Audio-Bridge-Contract.md). No engine
            // action happens at construction — the engine opens only when
            // the audio_init command runs.
            app.manage(AudioBridge::new(app.handle().clone()));
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::get_app_version,
            commands::get_sample_track,
            commands::audio_status,
            commands::audio_init,
            commands::audio_start,
            commands::audio_stop,
            commands::audio_shutdown,
            commands::audio_get_stats,
            commands::audio_is_deck_playing,
            commands::audio_play_pause,
            commands::audio_get_deck_state,
            commands::audio_get_deck_position,
            commands::audio_get_deck_duration,
            commands::audio_load_track,
            commands::audio_prepare_deck,
            commands::audio_play,
            commands::audio_pause,
            commands::audio_stop_deck,
            commands::audio_seek,
            commands::audio_set_volume,
            commands::audio_set_eq,
            commands::audio_set_filter,
            commands::audio_set_tempo_ratio,
            commands::audio_set_pitch_preservation,
            commands::audio_set_stem_levels,
            commands::audio_execute_transition
        ])
}
