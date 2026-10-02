// Whisper's stock inventions on silence and noise; its loops (below).
const JUNK = /^(you|thank you|thanks for watching|thank you for watching|thank you so much for watching|please subscribe|subtitles by .*|.*amara\.org.*)$/i;

/**
 * Tidies a speech-to-text result: drops bracketed sound tags, and returns ''
 * for what is not speech -- Whisper's stock phrases, a phrase repeated 4+
 * times in a row (it got stuck in a loop), or nothing but punctuation.
 */
export function cleanTranscript(raw: string | null | undefined): string {
  const t = String(raw || '').replace(/\[[^\]]*\]|\([^)]*\)|\*[^*]*\*|♪/g, ' ').replace(/\s+/g, ' ').trim();
  if (/(^|\s)(\S+(?:\s+\S+){0,3}?)(?:[\s,.!?]+\2(?=[\s,.!?]|$)){3,}/i.test(t)) return '';
  if (!/[0-9A-Za-zÀ-￿]/.test(t)) return '';
  return JUNK.test(t.replace(/[\s.!?,…]+$/, '')) ? '' : t;
}
