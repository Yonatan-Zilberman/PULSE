import { create } from 'zustand';

export interface TrackMetadata {
  id: string;
  title: string;
  artist: string;
  bpm: number;
  key: string;
  duration: number;
  energy: number;
  artwork?: string;
}

export interface DeckState {
  id: 'A' | 'B';
  track: TrackMetadata | null;
  isPlaying: boolean;
  playbackPosition: number; // in seconds
  volume: number; // 0.0 to 1.0
  lowEq: number; // -24dB to +6dB normalized (-1.0 to 1.0)
  midEq: number;
  highEq: number;
  filter: number; // -1.0 (LPF) to 1.0 (HPF), 0.0 is neutral
  stems: {
    vocals: number;
    drums: number;
    bass: number;
    other: number;
  };
}

export interface TransitionPreviewState {
  sourceDeck: 'A' | 'B';
  destinationDeck: 'A' | 'B';
  strategy: 'EQ_CROSSFADE' | 'BASS_SWAP' | 'STEM_ISOLATION' | 'DROP_SWAP' | 'ECHO_OUT';
  crossfader: number; // -1.0 (Deck A) to 1.0 (Deck B)
  confidence: number; // 0.0 to 1.0
  barsRemaining: number;
}

export interface AppState {
  // Deck states
  deckA: DeckState;
  deckB: DeckState;

  // AutoDJ Master State
  isAutoDJActive: boolean;
  targetEnergy: number; // 1 to 10
  setDurationMinutes: number;

  // Queue
  queue: TrackMetadata[];

  // Active Transition
  activeTransition: TransitionPreviewState | null;

  // Actions
  togglePlayDeck: (deck: 'A' | 'B') => void;
  loadTrackToDeck: (deck: 'A' | 'B', track: TrackMetadata) => void;
  setDeckVolume: (deck: 'A' | 'B', volume: number) => void;
  setCrossfader: (position: number) => void;
  toggleAutoDJ: () => void;
  setTargetEnergy: (energy: number) => void;
  addToQueue: (track: TrackMetadata) => void;
  removeFromQueue: (trackId: string) => void;
}

const initialDeckState = (id: 'A' | 'B'): DeckState => ({
  id,
  track: null,
  isPlaying: false,
  playbackPosition: 0,
  volume: 1.0,
  lowEq: 0,
  midEq: 0,
  highEq: 0,
  filter: 0,
  stems: { vocals: 1.0, drums: 1.0, bass: 1.0, other: 1.0 },
});

export const useAppStore = create<AppState>((set) => ({
  deckA: initialDeckState('A'),
  deckB: initialDeckState('B'),
  isAutoDJActive: true,
  targetEnergy: 7,
  setDurationMinutes: 60,
  queue: [],
  activeTransition: {
    sourceDeck: 'A',
    destinationDeck: 'B',
    strategy: 'BASS_SWAP',
    crossfader: -1.0,
    confidence: 0.94,
    barsRemaining: 16,
  },

  togglePlayDeck: (deck) =>
    set((state) => ({
      [deck === 'A' ? 'deckA' : 'deckB']: {
        ...state[deck === 'A' ? 'deckA' : 'deckB'],
        isPlaying: !state[deck === 'A' ? 'deckA' : 'deckB'].isPlaying,
      },
    })),

  loadTrackToDeck: (deck, track) =>
    set((state) => ({
      [deck === 'A' ? 'deckA' : 'deckB']: {
        ...state[deck === 'A' ? 'deckA' : 'deckB'],
        track,
        playbackPosition: 0,
        isPlaying: false,
      },
    })),

  setDeckVolume: (deck, volume) =>
    set((state) => ({
      [deck === 'A' ? 'deckA' : 'deckB']: {
        ...state[deck === 'A' ? 'deckA' : 'deckB'],
        volume: Math.max(0, Math.min(1, volume)),
      },
    })),

  setCrossfader: (position) =>
    set((state) => ({
      activeTransition: state.activeTransition
        ? { ...state.activeTransition, crossfader: position }
        : null,
    })),

  toggleAutoDJ: () => set((state) => ({ isAutoDJActive: !state.isAutoDJActive })),

  setTargetEnergy: (energy) => set({ targetEnergy: Math.max(1, Math.min(10, energy)) }),

  addToQueue: (track) => set((state) => ({ queue: [...state.queue, track] })),

  removeFromQueue: (trackId) =>
    set((state) => ({ queue: state.queue.filter((t) => t.id !== trackId) })),
}));
