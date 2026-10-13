// Browser equivalent of patcher.py: apply the patch to the user's own copy
// of Rossum's software file, entirely on their computer.
const APP_HEADER_BYTES = 8;   // file bytes, load address
const WORD_BYTES = 4;
const BUILD_ID_BYTES = 8;
const MAX_APP_BYTES = 4000000;
const EXTENSION_MAGIC = 'SQA8EXT1';
// ARM branches a hook may be, with a 24-bit word offset.
const BRANCH_OPCODES = {b: 0xea, bl: 0xeb, beq: 0x0a};
const BRANCH_PIPELINE_BYTES = 8;
const BRANCH_OFFSET_BITS = 24;
// Zip records (PKWARE APPNOTE): signatures and the fields read or written here.
const ZIP_LOCAL = 0x04034b50, ZIP_CENTRAL = 0x02014b50, ZIP_END = 0x06054b50;
const ZIP_LOCAL_BYTES = 30, ZIP_CENTRAL_BYTES = 46, ZIP_END_BYTES = 22;
const ZIP_STORED = 0, ZIP_DEFLATED = 8, ZIP_VERSION = 20;

function require(condition, message) {
  if (!condition) throw new Error(message);
}
function fields(object, names, optional = '') {
  if (object === null || typeof object !== 'object' || Array.isArray(object)) return false;
  const allowed = new Set([...names.split(','), ...optional.split(',').filter(Boolean)]);
  return names.split(',').every(name => name in object) && Object.keys(object).every(key => allowed.has(key));
}
function number(value) {
  if (Number.isInteger(value)) return value;
  require(typeof value === 'string' && /^0x[0-9a-f]+$/i.test(value), 'Invalid number in patch.');
  return parseInt(value, 16);
}
function fromHex(text) {
  require(typeof text === 'string' && /^(?:[0-9a-f]{2})*$/.test(text), 'Invalid data in patch.');
  return Uint8Array.from(text.match(/../g) ?? [], pair => parseInt(pair, 16));
}
const toHex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
export async function sha256(bytes) {
  return toHex(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)));
}
const CRC_TABLE = Uint32Array.from({length: 256}, (_, n) => {
  for (let bit = 0; bit < 8; bit++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

// Rossum's download is a zip that holds one file, `app`.
async function unzipApp(zip) {
  const view = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  let end = zip.length - ZIP_END_BYTES;
  while (end >= 0 && view.getUint32(end, true) !== ZIP_END) end--;
  require(end >= 0, 'This zip is not Rossum’s Assimil8or software download.');
  let at = view.getUint32(end + 16, true), found = null;
  for (let n = view.getUint16(end + 10, true); n > 0; n--) {
    require(at + ZIP_CENTRAL_BYTES <= zip.length && view.getUint32(at, true) === ZIP_CENTRAL, 'Invalid zip.');
    const nameBytes = view.getUint16(at + 28, true);
    const name = new TextDecoder().decode(zip.subarray(at + ZIP_CENTRAL_BYTES, at + ZIP_CENTRAL_BYTES + nameBytes));
    if (!name.endsWith('/')) {
      require(name === 'app' && !found, 'This zip is not Rossum’s Assimil8or software download.');
      found = {method: view.getUint16(at + 10, true), packed: view.getUint32(at + 20, true),
               size: view.getUint32(at + 24, true), local: view.getUint32(at + 42, true)};
    }
    at += ZIP_CENTRAL_BYTES + nameBytes + view.getUint16(at + 30, true) + view.getUint16(at + 32, true);
  }
  require(found && found.size <= MAX_APP_BYTES && view.getUint32(found.local, true) === ZIP_LOCAL, 'Invalid zip.');
  const start = found.local + ZIP_LOCAL_BYTES + view.getUint16(found.local + 26, true) + view.getUint16(found.local + 28, true);
  const packed = zip.subarray(start, start + found.packed);
  if (found.method === ZIP_STORED) return packed.slice();
  require(found.method === ZIP_DEFLATED && typeof DecompressionStream === 'function',
    'Use a current browser, or unzip the download and choose the app file inside it.');
  const stream = new Blob([packed]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  const app = new Uint8Array(await new Response(stream).arrayBuffer());
  require(app.length === found.size, 'Invalid zip.');
  return app;
}

function branchTarget(address, word, instruction) {
  const value = new DataView(word.buffer, word.byteOffset).getUint32(0, true);
  require(value >>> 24 === BRANCH_OPCODES[instruction], 'Patch hook is not the expected branch.');
  let offset = value & ((1 << BRANCH_OFFSET_BITS) - 1);
  if (offset >>> (BRANCH_OFFSET_BITS - 1)) offset -= 1 << BRANCH_OFFSET_BITS;
  return address + BRANCH_PIPELINE_BYTES + offset * WORD_BYTES;
}

// Check a patch against the layout: it may only fill the overlay region,
// replace the listed instructions with branches into it, and copy the listed
// stock ranges.
export function decodePatch(bundle) {
  require(fields(bundle, 'patch,layout'), 'Invalid patch bundle.');
  const {patch, layout} = bundle;
  require(fields(patch, 'format,base_sha256,overlay,hooks,splices,extension', 'result') && patch.format === 1 &&
    patch.base_sha256 === layout.firmware.app_sha256, 'Unsupported patch.');
  const start = number(layout.overlay.start), end = number(layout.overlay.end);
  const overlay = fromHex(patch.overlay);
  require(overlay.length > 0 && overlay.length % WORD_BYTES === 0 && overlay.length <= end - start,
    'Patch overlay escapes its region.');

  const wanted = new Map(layout.hooks.map(hook => [number(hook.address), hook.instruction]));
  const hooks = new Map();
  require(Array.isArray(patch.hooks) && patch.hooks.length === wanted.size, 'Invalid patch hook count.');
  for (const hook of patch.hooks) {
    require(fields(hook, 'address,word'), 'Invalid patch hook.');
    const address = number(hook.address), word = fromHex(hook.word);
    require(wanted.has(address) && !hooks.has(address) && word.length === WORD_BYTES, 'Patch hook outside the layout.');
    const target = branchTarget(address, word, wanted.get(address));
    require(target >= start && target < start + overlay.length, 'Patch hook leaves the overlay.');
    hooks.set(address, word);
  }

  require(Array.isArray(patch.splices) && patch.splices.length === layout.splices.length, 'Invalid patch splice count.');
  const splices = patch.splices.map((splice, index) => {
    const allowed = layout.splices[index].parts;
    require(fields(splice, 'address,parts') && Array.isArray(splice.parts) && splice.parts.length === allowed.length,
      'Patch splice differs from the layout.');
    const address = number(splice.address);
    let size = 0;
    const parts = splice.parts.map((part, n) => {
      if ('stock' in allowed[n]) {
        require(fields(part, 'stock') && JSON.stringify(part.stock) === JSON.stringify(allowed[n].stock),
          'Patch splice copies an unlisted range.');
        size += number(part.stock[1]);
        return {stock: number(part.stock[0]), bytes: number(part.stock[1])};
      }
      require(fields(part, 'data'), 'Invalid patch splice part.');
      const data = fromHex(part.data);
      size += data.length;
      return {data};
    });
    require(address >= start && address + size <= start + overlay.length, 'Patch splice escapes the overlay.');
    return {address, parts};
  });

  require(fields(patch.extension, 'image'), 'Invalid patch extension.');
  const extension = fromHex(patch.extension.image);
  const header = layout.extension;
  require(extension.length >= header.header_bytes &&
    new TextDecoder().decode(extension.subarray(0, EXTENSION_MAGIC.length)) === EXTENSION_MAGIC, 'Invalid patch extension.');
  const view = new DataView(extension.buffer);
  require(view.getUint32(header.file_bytes_offset, true) === extension.length &&
    extension.length + view.getUint32(header.file_bytes_offset + WORD_BYTES, true) <= header.max_bytes,
    'Patch extension exceeds its reserved range.');
  require(patch.result === undefined || fields(patch.result, 'build_id,app_sha256,extension_sha256'), 'Invalid patch result.');
  return {overlay, hooks, splices, extension, result: patch.result};
}

// Returns the patched `app`, its extension image and the build ID.
export async function patchFirmware(file, bundle) {
  const {layout} = bundle;
  const patch = decodePatch(bundle);
  const stock = file[0] === 0x50 && file[1] === 0x4b ? await unzipApp(file) : file;
  require(stock.length === layout.firmware.app_bytes && await sha256(stock) === layout.firmware.app_sha256,
    `Choose Rossum’s unmodified Assimil8or v${layout.firmware.version} download: the zip, or the app file inside it.`);
  const base = number(layout.firmware.load_address) - APP_HEADER_BYTES;  // address of file offset 0
  const start = number(layout.overlay.start), end = number(layout.overlay.end);
  const image = stock.slice();
  for (const [address, word] of patch.hooks) image.set(word, address - base);
  image.fill(0, start - base, end - base);
  image.set(patch.overlay, start - base);
  for (const {address, parts} of patch.splices) {
    let at = address - base;
    for (const part of parts) {
      const chunk = part.data ?? stock.subarray(part.stock - base, part.stock - base + part.bytes);
      image.set(chunk, at);
      at += chunk.length;
    }
  }
  // The identity record: the build ID is the image's hash with the record's
  // ID and CRC zero, and the CRC covers the image with only itself zero.
  const payload = image.subarray(APP_HEADER_BYTES);
  const buildId = new Uint8Array(await crypto.subtle.digest('SHA-256', payload)).subarray(0, BUILD_ID_BYTES);
  image.set(buildId, start - base + layout.overlay.build_id_offset);
  new DataView(image.buffer).setUint32(start - base + layout.overlay.crc_offset, crc32(payload), true);
  // The extension names the build it links against and carries its own CRC.
  const extension = patch.extension.slice();
  extension.set(buildId, layout.extension.build_id_offset);
  new DataView(extension.buffer).setUint32(layout.extension.crc_offset, crc32(extension), true);
  const id = toHex(buildId);
  if (patch.result) {
    require(patch.result.build_id === id && patch.result.app_sha256 === await sha256(image) &&
      patch.result.extension_sha256 === await sha256(extension), 'The patched files failed verification.');
  }
  return {app: image, extension, buildId: id, extensionName: layout.extension.name.replace('{build_id}', id)};
}

// A zip of stored files, for one download holding both.
export function zipFiles(files) {
  const records = [], central = [];
  let offset = 0;
  for (const {name, data} of files) {
    const nameBytes = new TextEncoder().encode(name), crc = crc32(data);
    const local = new Uint8Array(ZIP_LOCAL_BYTES + nameBytes.length);
    const view = new DataView(local.buffer);
    view.setUint32(0, ZIP_LOCAL, true); view.setUint16(4, ZIP_VERSION, true); view.setUint16(8, ZIP_STORED, true);
    view.setUint32(14, crc, true); view.setUint32(18, data.length, true); view.setUint32(22, data.length, true);
    view.setUint16(26, nameBytes.length, true);
    local.set(nameBytes, ZIP_LOCAL_BYTES);
    const entry = new Uint8Array(ZIP_CENTRAL_BYTES + nameBytes.length);
    const entryView = new DataView(entry.buffer);
    entryView.setUint32(0, ZIP_CENTRAL, true); entryView.setUint16(4, ZIP_VERSION, true); entryView.setUint16(6, ZIP_VERSION, true);
    entryView.setUint16(10, ZIP_STORED, true); entryView.setUint32(16, crc, true);
    entryView.setUint32(20, data.length, true); entryView.setUint32(24, data.length, true);
    entryView.setUint16(28, nameBytes.length, true); entryView.setUint32(42, offset, true);
    entry.set(nameBytes, ZIP_CENTRAL_BYTES);
    records.push(local, data); central.push(entry);
    offset += local.length + data.length;
  }
  const directoryBytes = central.reduce((sum, entry) => sum + entry.length, 0);
  const end = new Uint8Array(ZIP_END_BYTES), endView = new DataView(end.buffer);
  endView.setUint32(0, ZIP_END, true); endView.setUint16(8, files.length, true); endView.setUint16(10, files.length, true);
  endView.setUint32(12, directoryBytes, true); endView.setUint32(16, offset, true);
  const zip = new Uint8Array(offset + directoryBytes + ZIP_END_BYTES);
  let at = 0;
  for (const piece of [...records, ...central, end]) { zip.set(piece, at); at += piece.length; }
  return zip;
}
