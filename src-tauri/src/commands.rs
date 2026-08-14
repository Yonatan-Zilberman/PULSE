use crate::models::TrackMetadata;

#[tauri::command]
pub fn get_app_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}

#[tauri::command]
pub fn ping_audio_engine() -> bool {
    // Returns status of the audio engine
    true
}

#[tauri::command]
pub fn get_sample_track() -> TrackMetadata {
    TrackMetadata {
        title: "PULSE Baseline".to_string(),
        artist: "Core Engine".to_string(),
        album: Some("Zero Cloud Initializer".to_string()),
        genre: Some("Electronic".to_string()),
        duration_seconds: 300.0,
        artwork_uri: None,
        file_path: "/dummy/path.wav".to_string(),
        sample_rate: 48000,
        channels: 2,
    }
}
