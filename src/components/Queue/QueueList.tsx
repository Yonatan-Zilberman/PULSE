import React from 'react';
import { useAppStore } from '../../state/useAppStore';
import { ListMusic, Trash2 } from 'lucide-react';

export const QueueList: React.FC = () => {
  const { queue, removeFromQueue } = useAppStore();

  return (
    <div
      className="glass-card"
      style={{ padding: '1rem', height: '100%', display: 'flex', flexDirection: 'column' }}
    >
      <div
        style={{ display: 'flex', alignItems: 'center', gap: '0.5rem', marginBottom: '0.75rem' }}
      >
        <ListMusic size={18} color="var(--text-secondary)" />
        <h3 style={{ fontSize: '1rem', fontWeight: 600 }}>DJ Brain Queue ({queue.length})</h3>
      </div>

      {queue.length === 0 ? (
        <div
          style={{
            flex: 1,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            color: 'var(--text-muted)',
            fontSize: '0.875rem',
            border: '1px dashed rgba(255, 255, 255, 0.1)',
            borderRadius: '8px',
            padding: '1rem',
          }}
        >
          Queue is empty. DJ Brain will pick autonomous transitions.
        </div>
      ) : (
        <div style={{ display: 'flex', flexDirection: 'column', gap: '0.5rem', overflowY: 'auto' }}>
          {queue.map((track, idx) => (
            <div
              key={track.id}
              style={{
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'space-between',
                padding: '0.5rem 0.75rem',
                backgroundColor: 'rgba(255, 255, 255, 0.03)',
                borderRadius: '6px',
                border: '1px solid rgba(255, 255, 255, 0.05)',
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: '0.75rem' }}>
                <span style={{ fontSize: '0.75rem', color: 'var(--text-muted)', width: '16px' }}>
                  {idx + 1}
                </span>
                <div>
                  <div style={{ fontSize: '0.875rem', fontWeight: 500 }}>{track.title}</div>
                  <div style={{ fontSize: '0.75rem', color: 'var(--text-secondary)' }}>
                    {track.artist} • {track.bpm} BPM
                  </div>
                </div>
              </div>

              <button
                onClick={() => removeFromQueue(track.id)}
                style={{
                  background: 'none',
                  border: 'none',
                  color: 'var(--text-muted)',
                  cursor: 'pointer',
                  padding: '4px',
                }}
                title="Remove from queue"
              >
                <Trash2 size={14} />
              </button>
            </div>
          ))}
        </div>
      )}
    </div>
  );
};
