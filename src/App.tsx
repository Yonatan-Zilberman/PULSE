import React from 'react';
import { useAppStore } from './state/useAppStore';
import { AutoDJControls } from './components/AutoDJ/AutoDJControls';
import { WaveformDisplay } from './components/Waveform/WaveformDisplay';
import { QueueList } from './components/Queue/QueueList';
import { TransitionCard } from './components/TransitionPreview/TransitionCard';
import { EnergyCurveViewer } from './components/EnergyCurve/EnergyCurveViewer';
import { MixerStrip } from './components/Mixer/MixerStrip';
import { Disc3, Play, Pause, Radio, Cpu } from 'lucide-react';

export const App: React.FC = () => {
  const { deckA, deckB, togglePlayDeck } = useAppStore();

  return (
    <div
      style={{
        display: 'flex',
        flexDirection: 'column',
        height: '100vh',
        padding: '1.25rem',
        gap: '1rem',
      }}
    >
      {/* Top Header */}
      <header style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '0.75rem' }}>
          <div
            style={{
              width: '36px',
              height: '36px',
              borderRadius: '8px',
              background: 'linear-gradient(135deg, var(--deck-a), var(--deck-b))',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              color: '#000',
              fontWeight: 900,
            }}
          >
            P
          </div>
          <div>
            <h1 style={{ fontSize: '1.25rem', fontWeight: 800, letterSpacing: '-0.025em' }}>
              PULSE
            </h1>
            <p style={{ fontSize: '0.75rem', color: 'var(--text-secondary)' }}>
              Autonomous Local DJ Engine
            </p>
          </div>
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: '1rem' }}>
          <div
            style={{
              display: 'flex',
              alignItems: 'center',
              gap: '0.5rem',
              fontSize: '0.75rem',
              color: 'var(--accent-green)',
            }}
          >
            <Radio size={14} />
            <span>JUCE 8 CoreAudio: OK</span>
          </div>
          <div
            style={{
              display: 'flex',
              alignItems: 'center',
              gap: '0.5rem',
              fontSize: '0.75rem',
              color: 'var(--deck-a)',
            }}
          >
            <Cpu size={14} />
            <span>Apple Silicon M-Series</span>
          </div>
        </div>
      </header>

      {/* AutoDJ Master Bar */}
      <AutoDJControls />

      {/* Main DJ Surface */}
      <div
        style={{
          display: 'grid',
          gridTemplateColumns: '1fr 1fr 340px',
          gap: '1rem',
          flex: 1,
          minHeight: 0,
        }}
      >
        {/* Deck A Panel */}
        <div
          className={`glass-card ${deckA.isPlaying ? 'deck-a-glow' : ''}`}
          style={{ padding: '1.25rem', display: 'flex', flexDirection: 'column', gap: '1rem' }}
        >
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
              <Disc3 size={20} color="var(--deck-a)" />
              <h2 style={{ fontSize: '1.125rem', fontWeight: 700, color: 'var(--deck-a)' }}>
                DECK A
              </h2>
            </div>
            <button
              onClick={() => togglePlayDeck('A')}
              style={{
                padding: '0.4rem 0.8rem',
                borderRadius: '6px',
                backgroundColor: deckA.isPlaying ? 'var(--deck-a)' : 'rgba(255, 255, 255, 0.1)',
                color: deckA.isPlaying ? '#000' : 'var(--text-primary)',
                border: 'none',
                cursor: 'pointer',
                display: 'flex',
                alignItems: 'center',
                gap: '0.4rem',
                fontWeight: 600,
              }}
            >
              {deckA.isPlaying ? <Pause size={14} /> : <Play size={14} />}
              <span>{deckA.isPlaying ? 'PLAYING' : 'CUE'}</span>
            </button>
          </div>

          <WaveformDisplay deck={deckA} />
          <MixerStrip deck={deckA} />
        </div>

        {/* Deck B Panel */}
        <div
          className={`glass-card ${deckB.isPlaying ? 'deck-b-glow' : ''}`}
          style={{ padding: '1.25rem', display: 'flex', flexDirection: 'column', gap: '1rem' }}
        >
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
              <Disc3 size={20} color="var(--deck-b)" />
              <h2 style={{ fontSize: '1.125rem', fontWeight: 700, color: 'var(--deck-b)' }}>
                DECK B
              </h2>
            </div>
            <button
              onClick={() => togglePlayDeck('B')}
              style={{
                padding: '0.4rem 0.8rem',
                borderRadius: '6px',
                backgroundColor: deckB.isPlaying ? 'var(--deck-b)' : 'rgba(255, 255, 255, 0.1)',
                color: deckB.isPlaying ? '#000' : 'var(--text-primary)',
                border: 'none',
                cursor: 'pointer',
                display: 'flex',
                alignItems: 'center',
                gap: '0.4rem',
                fontWeight: 600,
              }}
            >
              {deckB.isPlaying ? <Pause size={14} /> : <Play size={14} />}
              <span>{deckB.isPlaying ? 'PLAYING' : 'CUE'}</span>
            </button>
          </div>

          <WaveformDisplay deck={deckB} />
          <MixerStrip deck={deckB} />
        </div>

        {/* Right Sidebar: Queue & Intelligence */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: '1rem', minHeight: 0 }}>
          <QueueList />
          <EnergyCurveViewer />
        </div>
      </div>

      {/* Bottom Transition Deck */}
      <TransitionCard />
    </div>
  );
};

export default App;
