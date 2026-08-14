import React from 'react';
import { DeckState, useAppStore } from '../../state/useAppStore';
import { Sliders } from 'lucide-react';

interface MixerStripProps {
  deck: DeckState;
}

export const MixerStrip: React.FC<MixerStripProps> = ({ deck }) => {
  const { setDeckVolume } = useAppStore();
  const accentColor = deck.id === 'A' ? 'var(--deck-a)' : 'var(--deck-b)';

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '0.5rem', width: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <span
          style={{
            fontSize: '0.75rem',
            color: 'var(--text-secondary)',
            display: 'flex',
            alignItems: 'center',
            gap: '4px',
          }}
        >
          <Sliders size={12} /> Volume
        </span>
        <span style={{ fontSize: '0.75rem', fontWeight: 600, color: accentColor }}>
          {Math.round(deck.volume * 100)}%
        </span>
      </div>

      <input
        type="range"
        min="0"
        max="1"
        step="0.01"
        value={deck.volume}
        onChange={(e) => setDeckVolume(deck.id, parseFloat(e.target.value))}
        style={{ width: '100%', accentColor, cursor: 'pointer' }}
      />
    </div>
  );
};
