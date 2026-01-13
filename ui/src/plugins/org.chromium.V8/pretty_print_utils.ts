export function computePositionMapping(
  original: string,
  formatted: string,
): Int32Array {
  const map = new Int32Array(original.length).fill(-1);
  let j = 0;
  for (let i = 0; i < original.length; i++) {
    if (/\s/.test(original[i])) continue;

    // Scan ahead in formatted to find match
    let found = -1;
    for (let k = j; k < Math.min(formatted.length, j + 200); k++) {
      if (charsMatch(original[i], formatted[k])) {
        found = k;
        break;
      }
    }

    if (found !== -1) {
      map[i] = found;
      j = found + 1;
    }
  }
  return map;
}

function charsMatch(c1: string, c2: string): boolean {
  if (c1 === c2) return true;
  if ((c1 === '"' || c1 === "'") && (c2 === '"' || c2 === "'")) return true;
  return false;
}
