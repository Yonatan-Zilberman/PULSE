import { describe, it, expect, beforeEach } from 'vitest';
import { useAppStore, TrackMetadata } from './useAppStore';

describe('useAppStore Unit Tests', () => {
  beforeEach(() => {
    useAppStore.setState({
      deckA: {
        id: 'A',
        track: null,
        isPlaying: false,
        playbackPosition: 0,
        volume: 1.0,
        lowEq: 0,
        midEq: 0,
        highEq: 0,
        filter: 0,
        stems: { vocals: 1.0, drums: 1.0, bass: 1.0, other: 1.0 },
      },
      queue: [],
      isAutoDJActive: true,
      targetEnergy: 7,
    });
  });

  it('initializes with default values', () => {
    const state = useAppStore.getState();
    expect(state.deckA.id).toBe('A');
    expect(state.deckB.id).toBe('B');
    expect(state.isAutoDJActive).toBe(true);
    expect(state.targetEnergy).toBe(7);
  });

  it('toggles playback on a deck', () => {
    const { togglePlayDeck } = useAppStore.getState();
    togglePlayDeck('A');
    expect(useAppStore.getState().deckA.isPlaying).toBe(true);
    togglePlayDeck('A');
    expect(useAppStore.getState().deckA.isPlaying).toBe(false);
  });

  it('loads track onto deck', () => {
    const { loadTrackToDeck } = useAppStore.getState();
    const mockTrack: TrackMetadata = {
      id: 'trk-1',
      title: 'Midnight Resonance',
      artist: 'Pulse Core',
      bpm: 126.0,
      key: '8A',
      duration: 340.0,
      energy: 8,
    };
    loadTrackToDeck('A', mockTrack);
    expect(useAppStore.getState().deckA.track).toEqual(mockTrack);
    expect(useAppStore.getState().deckA.playbackPosition).toBe(0);
  });

  it('adds and removes tracks from queue', () => {
    const { addToQueue, removeFromQueue } = useAppStore.getState();
    const mockTrack: TrackMetadata = {
      id: 'trk-2',
      title: 'Sub Zero',
      artist: 'Deep State',
      bpm: 128.0,
      key: '9A',
      duration: 310.0,
      energy: 9,
    };

    addToQueue(mockTrack);
    expect(useAppStore.getState().queue).toHaveLength(1);
    expect(useAppStore.getState().queue[0].id).toBe('trk-2');

    removeFromQueue('trk-2');
    expect(useAppStore.getState().queue).toHaveLength(0);
  });
});
