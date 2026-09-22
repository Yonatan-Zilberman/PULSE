//! Rust boundary for the C++ real-time audio engine.
//!
//! The stable contract — frozen C ABI, event kinds, ownership, thread
//! ownership, memory lifetime, cancellation, error codes, and the
//! `pulse://audio-event` payload — is documented in
//! `Docs/Audio-Bridge-Contract.md`.
//!
//! Boundary rule: no production code outside [`bridge`] may call [`ffi`].
//! The C FFI is private to the bridge; all commands go through
//! [`bridge::AudioBridge`]. (Integration tests may exercise the raw FFI
//! contract directly to assert C-side behavior.)

pub mod bridge;
pub mod errors;
pub mod ffi;
pub mod plan;
pub mod types;

pub use bridge::{
    AudioBridge, DeckState, EngineConfig, EnginePhase, EngineStats, EventSink, TauriSink, VecSink,
    EVENT_NAME,
};
pub use errors::AudioBridgeError;
pub use plan::TransitionPlan;
pub use types::*;
