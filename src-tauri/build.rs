use std::path::Path;
use std::process::Command;

fn main() {
    tauri_build::build();

    // Always register the custom cfg so the `unexpected_cfgs` lint accepts
    // `#[cfg(pulse_audio_engine_unavailable)]` in either build variant.
    println!("cargo:rustc-check-cfg=cfg(pulse_audio_engine_unavailable)");

    // Re-run when the C++ tree changes (small tree; explicit directory entries).
    let manifest_dir =
        std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR is set by cargo");
    let repo_root = Path::new(&manifest_dir)
        .parent()
        .expect("src-tauri must live directly under the repository root");
    let src_cpp = repo_root.join("src-cpp");
    println!(
        "cargo:rerun-if-changed={}",
        src_cpp.join("CMakeLists.txt").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        src_cpp.join("include").display()
    );
    println!("cargo:rerun-if-changed={}", src_cpp.join("src").display());
    // Allow recovery when CMake appears/disappears from the environment.
    println!("cargo:rerun-if-env-changed=PATH");

    let build_dir = Path::new(&std::env::var("OUT_DIR").expect("OUT_DIR is set by cargo"))
        .join("pulse_audio_engine_cxx");

    // Configure: a spawn error means CMake is not on PATH -> degrade gracefully
    // (build succeeds; Rust FFI gets stubs and commands report EngineNotAvailable).
    let configure = Command::new("cmake")
        .arg("-B")
        .arg(&build_dir)
        .arg("-S")
        .arg(&src_cpp)
        .arg("-DCMAKE_BUILD_TYPE=Release")
        .output();
    match configure {
        Ok(configure) if configure.status.success() => {}
        Ok(configure) => {
            let stderr = String::from_utf8_lossy(&configure.stderr);
            eprintln!("build.rs: CMake configure failed:\n{stderr}");
            panic!("CMake configure of src-cpp failed (see stderr above)");
        }
        Err(err) => {
            println!(
                "cargo:warning=cmake not found on PATH ({err}); \
                 building without the C++ audio engine. Audio commands will report \
                 EngineNotAvailable at runtime."
            );
            println!("cargo:rustc-cfg=pulse_audio_engine_unavailable");
            return;
        }
    }

    // Build the static library only (skips CLI/test executables).
    // A present-but-failing CMake is a real build error: fail loudly.
    let build = Command::new("cmake")
        .arg("--build")
        .arg(&build_dir)
        .arg("--target")
        .arg("pulse_audio_engine")
        .output()
        .unwrap_or_else(|err| panic!("failed to run cmake --build: {err}"));
    if !build.status.success() {
        let stdout = String::from_utf8_lossy(&build.stdout);
        let stderr = String::from_utf8_lossy(&build.stderr);
        eprintln!("build.rs: CMake build failed:\n{stdout}\n{stderr}");
        panic!("CMake build of libpulse_audio_engine failed (see output above)");
    }

    // Link directives.
    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-lib=pulse_audio_engine");
    println!("cargo:rustc-link-lib=c++");
    if cfg!(target_os = "macos") {
        for framework in ["CoreAudio", "AudioToolbox", "Accelerate", "CoreFoundation"] {
            println!("cargo:rustc-link-lib=framework={framework}");
        }
    }
}
