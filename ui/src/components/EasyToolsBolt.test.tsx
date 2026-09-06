/* @vitest-environment jsdom */

import { describe, expect, it } from 'vitest';
import { render } from '@testing-library/react';
import { LogoGlyph, EasyToolsBolt } from './EasyToolsBolt';

describe('LogoGlyph & EasyToolsBolt Component', () => {
  it('renders with default props', () => {
    const { container } = render(<LogoGlyph />);
    const svg = container.querySelector('svg');
    expect(svg).toBeTruthy();
    expect(svg?.getAttribute('width')).toBe('24');
    expect(svg?.getAttribute('height')).toBe('24');
    expect(svg?.getAttribute('viewBox')).toBe('100 53 1110 1110');
    expect(svg?.getAttribute('aria-hidden')).toBe('true');

    // All 9 origami facet paths should exist and use default fill
    const paths = container.querySelectorAll('path');
    expect(paths.length).toBe(9);
    paths.forEach((p) => {
      expect(p.getAttribute('fill')).toBe('currentColor');
      expect(p.getAttribute('fill-opacity')).toBeTruthy();
    });
  });

  it('supports custom size and theme fill', () => {
    const { container } = render(
      <LogoGlyph size={18} fill="var(--primary)" className="brand-logo" />
    );
    const svg = container.querySelector('svg');
    expect(svg?.getAttribute('width')).toBe('18');
    expect(svg?.getAttribute('height')).toBe('18');
    expect(svg?.classList.contains('brand-logo')).toBe(true);

    const paths = container.querySelectorAll('path');
    expect(paths.length).toBe(9);
    paths.forEach((p) => {
      expect(p.getAttribute('fill')).toBe('var(--primary)');
    });
  });

  it('merges custom styles and preserves flexShrink', () => {
    const { container } = render(
      <LogoGlyph size={32} style={{ opacity: 0.8, transform: 'scale(1.1)' }} />
    );
    const svg = container.querySelector('svg') as unknown as SVGSVGElement;
    expect(svg.style.display).toBe('inline-block');
    expect(svg.style.flexShrink).toBe('0');
    expect(svg.style.opacity).toBe('0.8');
    expect(svg.style.transform).toBe('scale(1.1)');
  });

  it('exports EasyToolsBolt as an identical backward-compatible alias', () => {
    expect(EasyToolsBolt).toBe(LogoGlyph);
    const { container } = render(<EasyToolsBolt size={20} fill="#6366f1" />);
    const svg = container.querySelector('svg');
    expect(svg?.getAttribute('width')).toBe('20');
    const firstPath = container.querySelector('path');
    expect(firstPath?.getAttribute('fill')).toBe('#6366f1');
  });
});
