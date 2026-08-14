use serde::{Deserialize, Serialize};

#[repr(C)]
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct AudioEngineConfigC {
    pub sample_rate: u32,
    pub buffer_size: u32,
    pub channel_count: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct DeckStateC {
    pub deck_id: u8, // 0 = Deck A, 1 = Deck B
    pub is_playing: u8,
    pub playback_position_seconds: f64,
    pub volume: f32,
    pub low_eq: f32,
    pub mid_eq: f32,
    pub high_eq: f32,
    pub filter: f32,
    pub vocal_stem_vol: f32,
    pub drum_stem_vol: f32,
    pub bass_stem_vol: f32,
    pub other_stem_vol: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct TransitionCommandC {
    pub source_deck: u8,
    pub destination_deck: u8,
    pub duration_seconds: f64,
    pub transition_type: u32,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::size_of;

    #[test]
    fn test_c_struct_sizes() {
        assert_eq!(size_of::<AudioEngineConfigC>(), 12);
        // DeckStateC has f64 and multiple f32s; ensure predictable memory layout
        assert!(size_of::<DeckStateC>() >= 48);
    }
}
