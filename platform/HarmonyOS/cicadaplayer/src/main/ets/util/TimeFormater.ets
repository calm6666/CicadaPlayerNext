/*
 * Time formatting helpers, mirroring the Android demo's TimeFormater.
 */

/** Formats milliseconds as HH:MM:SS (or MM:SS when shorter than an hour). */
export function formatMs(ms: number): string {
  if (ms < 0 || !isFinite(ms)) {
    return '00:00';
  }
  const totalSeconds: number = Math.floor(ms / 1000);
  const seconds: number = totalSeconds % 60;
  const minutes: number = Math.floor(totalSeconds / 60) % 60;
  const hours: number = Math.floor(totalSeconds / 3600);
  const ss: string = seconds < 10 ? `0${seconds}` : `${seconds}`;
  const mm: string = minutes < 10 ? `0${minutes}` : `${minutes}`;
  if (hours > 0) {
    const hh: string = hours < 10 ? `0${hours}` : `${hours}`;
    return `${hh}:${mm}:${ss}`;
  }
  return `${mm}:${ss}`;
}

/** Formats a byte-per-second rate as a human readable speed. */
export function formatSpeed(bytesPerSecond: number): string {
  if (bytesPerSecond <= 0) {
    return '0 KB/s';
  }
  if (bytesPerSecond >= 1024 * 1024) {
    return `${(bytesPerSecond / 1024 / 1024).toFixed(2)} MB/s`;
  }
  if (bytesPerSecond >= 1024) {
    return `${(bytesPerSecond / 1024).toFixed(1)} KB/s`;
  }
  return `${bytesPerSecond} B/s`;
}
