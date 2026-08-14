import React from 'react';
import { useAppStore } from '../../state/useAppStore';
import { ArrowRightLeft, ShieldCheck } from 'lucide-react';

export const TransitionCard: React.FC = () => {
  const { activeTransition, setCrossfader } = useAppStore();

  if (!activeTransition) {
    return (
      <div
        className="glass-card"
        style={{ padding: '1rem', textAlign: 'center', color: 'var(--text-muted)' }}
      >
        No active transition scheduled.
      </div>
    );
  }

  return (
    <div
      className="glass-card"
      style={{ padding: '1rem', display: 'flex', flexDirection: 'column', gap: '0.75rem' }}
    >
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
          <ArrowRightLeft size={16} color="var(--accent-purple)" />
          <span style={{ fontSize: '0.875rem', fontWeight: 600 }}>
            Planned: Deck {activeTransition.sourceDeck} → Deck {activeTransition.destinationDeck}
          </span>
          <span
            style={{
              fontSize: '0.75rem',
              backgroundColor: 'rgba(139, 92, 246, 0.2)',
              color: 'var(--accent-purple)',
              padding: '2px 6px',
              borderRadius: '4px',
              fontWeight: 500,
            }}
          >
            {activeTransition.strategy}
          </span>
        </div>

        <div
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: '0.25rem',
            color: 'var(--accent-green)',
            fontSize: '0.75rem',
          }}
        >
          <ShieldCheck size={14} />
          <span>Confidence: {Math.round(activeTransition.confidence * 100)}%</span>
        </div>
      </div>

      {/* Crossfader control preview */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '1rem' }}>
        <span style={{ fontSize: '0.75rem', color: 'var(--deck-a)', fontWeight: 600 }}>Deck A</span>
        <input
          type="range"
          min="-1"
          max="1"
          step="0.01"
          value={activeTransition.crossfader}
          onChange={(e) => setCrossfader(parseFloat(e.target.value))}
          style={{ flex: 1, accentColor: 'var(--accent-purple)', cursor: 'pointer' }}
        />
        <span style={{ fontSize: '0.75rem', color: 'var(--deck-b)', fontWeight: 600 }}>Deck B</span>
      </div>
    </div>
  );
};
