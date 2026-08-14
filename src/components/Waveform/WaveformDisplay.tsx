import React from 'react';
import { DeckState } from '../../state/useAppStore';

interface WaveformDisplayProps {
  deck: DeckState;
}

export const WaveformDisplay: React.FC<WaveformDisplayProps> = ({ deck }) => {
  const accentColor = deck.id === 'A' ? 'var(--deck-a)' : 'var(--deck-b)';

  return (
    <div
      style={{
        width: '100%',
        height: '80px',
        backgroundColor: 'rgba(0, 0, 0, 0.4)',
        borderRadius: '8px',
        position: 'relative',
        overflow: 'hidden',
        border: '1px solid rgba(255, 255, 255, 0.05)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
      }}
    >
      {/* Waveform placeholder canvas grid */}
      <div
        style={{
          position: 'absolute',
          top: 0,
          left: 0,
          right: 0,
          bottom: 0,
          backgroundImage: `linear-gradient(90deg, transparent 95%, ${accentColor} 100%), linear-gradient(0deg, rgba(255, 255, 255, 0.03) 1px, transparent 1px)`,
          backgroundSize: '32px 100%, 100% 16px',
          opacity: 0.3,
        }}
      />

      {/* Playhead indicator */}
      <div
        style={{
          position: 'absolute',
          left: '50%',
          top: 0,
          bottom: 0,
          width: '2px',
          backgroundColor: accentColor,
          boxShadow: `0 0 8px ${accentColor}`,
          zIndex: 2,
        }}
      />

      <span style={{ fontSize: '0.75rem', color: 'var(--text-muted)', zIndex: 1 }}>
        {deck.track ? `${deck.track.bpm} BPM • ${deck.track.key}` : `Deck ${deck.id} Idle`}
      </span>
    </div>
  );
};
