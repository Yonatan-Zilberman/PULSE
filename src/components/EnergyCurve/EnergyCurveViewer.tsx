import React from 'react';
import { Activity } from 'lucide-react';

export const EnergyCurveViewer: React.FC = () => {
  // 12 bars energy profile visualization placeholder
  const energyLevels = [4, 5, 6, 6, 7, 8, 9, 8, 7, 8, 9, 7];

  return (
    <div
      className="glass-card"
      style={{ padding: '1rem', display: 'flex', flexDirection: 'column', gap: '0.5rem' }}
    >
      <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
        <Activity size={16} color="var(--accent-yellow)" />
        <span style={{ fontSize: '0.875rem', fontWeight: 600 }}>Set Energy Progression</span>
      </div>

      <div
        style={{
          display: 'flex',
          alignItems: 'flex-end',
          gap: '4px',
          height: '40px',
          paddingTop: '4px',
        }}
      >
        {energyLevels.map((lvl, idx) => (
          <div
            key={idx}
            style={{
              flex: 1,
              height: `${(lvl / 10) * 100}%`,
              backgroundColor: idx === 4 ? 'var(--accent-yellow)' : 'rgba(245, 158, 11, 0.3)',
              borderRadius: '2px',
              transition: 'height 0.3s ease',
            }}
            title={`Phase ${idx + 1}: Energy ${lvl}/10`}
          />
        ))}
      </div>
    </div>
  );
};
