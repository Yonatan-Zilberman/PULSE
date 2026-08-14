import React from 'react';
import { useAppStore } from '../../state/useAppStore';
import { Play, Pause, Zap, Compass } from 'lucide-react';

export const AutoDJControls: React.FC = () => {
  const { isAutoDJActive, toggleAutoDJ, targetEnergy, setTargetEnergy } = useAppStore();

  return (
    <div
      className="glass-card"
      style={{
        padding: '1rem',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: '1rem',
      }}
    >
      <div style={{ display: 'flex', alignItems: 'center', gap: '0.75rem' }}>
        <button
          onClick={toggleAutoDJ}
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: '0.5rem',
            padding: '0.5rem 1rem',
            borderRadius: '8px',
            backgroundColor: isAutoDJActive
              ? 'rgba(16, 185, 129, 0.2)'
              : 'rgba(255, 255, 255, 0.05)',
            border: `1px solid ${isAutoDJActive ? '#10b981' : 'rgba(255, 255, 255, 0.1)'}`,
            color: isAutoDJActive ? '#10b981' : 'var(--text-secondary)',
            cursor: 'pointer',
            fontWeight: 600,
          }}
        >
          {isAutoDJActive ? <Pause size={16} /> : <Play size={16} />}
          <span>{isAutoDJActive ? 'AUTODJ ACTIVE' : 'AUTODJ PAUSED'}</span>
        </button>

        <div
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: '0.25rem',
            color: 'var(--text-muted)',
            fontSize: '0.875rem',
          }}
        >
          <Compass size={14} />
          <span>Local Engine (Apple Silicon M-Series)</span>
        </div>
      </div>

      <div style={{ display: 'flex', alignItems: 'center', gap: '1rem' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
          <Zap size={16} color="#f59e0b" />
          <span style={{ fontSize: '0.875rem', color: 'var(--text-secondary)' }}>
            Target Energy:
          </span>
          <span style={{ fontWeight: 700, color: '#f59e0b' }}>{targetEnergy}/10</span>
        </div>
        <input
          type="range"
          min="1"
          max="10"
          value={targetEnergy}
          onChange={(e) => setTargetEnergy(Number(e.target.value))}
          style={{ width: '100px', accentColor: '#f59e0b', cursor: 'pointer' }}
        />
      </div>
    </div>
  );
};
