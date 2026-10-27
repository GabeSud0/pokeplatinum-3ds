// A ZIP of files kept as they are (stored, not compressed: the data pack
// hardly compresses, and storing is quick), as a Blob made of the files'
// own bytes, so nothing big is copied:
//
//   const blob = makeZip([{ name: "3ds/pokeplatinum.3dsx", data: bytes }, ...]);
//
// Names use "/" between folders; folders get their own entries. Every file
// must be under 4 GB, and the ZIP too (no ZIP64).

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

export function crc32(data) {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// 2020-01-01 00:00 in MS-DOS time: the same ZIP for the same files.
const DOS_TIME = 0;
const DOS_DATE = ((2020 - 1980) << 9) | (1 << 5) | 1;

export function makeZip(files) {
  const encoder = new TextEncoder();
  const entries = [];
  const seen = new Set();
  for (const { name } of files) {           // the folders, before their files
    const parts = name.split("/");
    for (let i = 1; i < parts.length; i++) {
      const folder = parts.slice(0, i).join("/") + "/";
      if (!seen.has(folder)) {
        seen.add(folder);
        entries.push({ name: folder, data: new Uint8Array(0) });
      }
    }
  }
  entries.push(...files);

  const parts = [];
  const central = [];
  let offset = 0;
  for (const { name, data } of entries) {
    const nameBytes = encoder.encode(name);
    const crc = crc32(data);
    const isFolder = name.endsWith("/");
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034b50, true);
    local.setUint16(4, 20, true);            // version needed: 2.0
    local.setUint16(6, 0x0800, true);        // names in UTF-8
    local.setUint16(8, 0, true);             // stored
    local.setUint16(10, DOS_TIME, true);
    local.setUint16(12, DOS_DATE, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, data.length, true);
    local.setUint32(22, data.length, true);
    local.setUint16(26, nameBytes.length, true);
    local.setUint16(28, 0, true);
    parts.push(local, nameBytes, data);

    const entry = new DataView(new ArrayBuffer(46));
    entry.setUint32(0, 0x02014b50, true);
    entry.setUint16(4, (3 << 8) | 20, true); // made by: Unix, 2.0
    entry.setUint16(6, 20, true);
    entry.setUint16(8, 0x0800, true);
    entry.setUint16(10, 0, true);
    entry.setUint16(12, DOS_TIME, true);
    entry.setUint16(14, DOS_DATE, true);
    entry.setUint32(16, crc, true);
    entry.setUint32(20, data.length, true);
    entry.setUint32(24, data.length, true);
    entry.setUint16(28, nameBytes.length, true);
    entry.setUint32(38, ((isFolder ? 0o40755 : 0o100644) << 16 | (isFolder ? 0x10 : 0)) >>> 0, true);
    entry.setUint32(42, offset, true);
    central.push(entry, nameBytes);
    offset += 30 + nameBytes.length + data.length;
  }
  const centralSize = central.reduce((n, p) => n + p.byteLength, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, entries.length, true);
  end.setUint16(10, entries.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);
  if (offset + centralSize > 0xffffffff) throw new Error("The ZIP would be over 4 GB");
  return new Blob([...parts, ...central, end], { type: "application/zip" });
}
