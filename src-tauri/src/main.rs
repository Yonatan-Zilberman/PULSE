// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    pulse_core_lib::create_app()
        .run(tauri::generate_context!())
        .expect("error while running PULSE tauri application");
}
