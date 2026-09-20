use crate::audio_bridge::{ffi, TransitionPlan};
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

/// Executes a fully precomputed transition plan on the C++ audio engine.
///
/// The plan is sanitized/mapped onto the versioned v2 `TransitionCommandC`
/// C ABI; returns `true` if the engine accepted it (structural validation).
#[tauri::command]
#[allow(unsafe_code, clippy::needless_pass_by_value)] // Tauri commands receive owned, deserialized arguments
pub fn execute_transition(plan: TransitionPlan) -> bool {
    let command = plan.into_transition_command();
    // SAFETY: `pulse_audio_execute_transition` is a plain C ABI function taking a
    // by-value POD struct; no pointers are involved and the engine validates it.
    unsafe { ffi::pulse_audio_execute_transition(command) == 0 }
}
