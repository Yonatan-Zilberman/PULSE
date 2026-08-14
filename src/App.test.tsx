import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import App from './App';

describe('App Component Smoke Test', () => {
  it('renders application title and deck headers', () => {
    render(<App />);
    expect(screen.getByText('PULSE')).toBeDefined();
    expect(screen.getByText('DECK A')).toBeDefined();
    expect(screen.getByText('DECK B')).toBeDefined();
    expect(screen.getByText('AUTODJ ACTIVE')).toBeDefined();
  });
});
