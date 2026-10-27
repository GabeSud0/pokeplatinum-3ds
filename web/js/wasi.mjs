// Runs a command-line tool compiled for WASI (WebAssembly's system
// interface, preview 1) in the browser or in Node, with its files in memory:
//
//   const { code, stdout, stderr, files } =
//       await runWasi(module, ["makerom", "-f", "cia", ...], { "/in.elf": bytes, ... });
//
// `module` is a compiled WebAssembly.Module; `files` maps absolute paths to
// Uint8Arrays (the tool sees them under "/", its only folder, which is also
// where relative paths start). Returns the exit code, what the tool printed,
// and every file afterwards (the inputs and what it wrote), as a Map.
//
// Only what bannertool and makerom (tools/build_cia_tools_wasm.sh) use is
// here: arguments, the clock, files and folders read and written; no
// environment variables, sockets or subprocesses.

const ERRNO = { SUCCESS: 0, BADF: 8, EXIST: 20, INVAL: 28, ISDIR: 31, NOENT: 44, NOSYS: 52, NOTDIR: 54, NOTEMPTY: 55 };
const FILETYPE = { CHARACTER_DEVICE: 2, DIRECTORY: 3, REGULAR_FILE: 4 };
const OFLAGS = { CREAT: 1, DIRECTORY: 2, EXCL: 4, TRUNC: 8 };
const FDFLAGS_APPEND = 1;
const ALL_RIGHTS = 0xffffffffffffffffn;

class Exit {
  constructor(code) { this.code = code; }
}

function normalize(path) {
  const parts = [];
  for (const part of path.split("/")) {
    if (part === "" || part === ".") continue;
    if (part === "..") parts.pop();
    else parts.push(part);
  }
  return "/" + parts.join("/");
}

export async function runWasi(module, args, files = {}) {
  // The file system: files by path, and the folders they imply.
  const fs = new Map();                       // path -> { data: Uint8Array, size }
  for (const [path, data] of Object.entries(files)) {
    fs.set(normalize(path), { data: new Uint8Array(data), size: data.length });
  }
  const isDir = (path) => {
    if (path === "/") return true;
    const prefix = path + "/";
    for (const p of fs.keys()) if (p.startsWith(prefix)) return true;
    return false;
  };
  const listDir = (path) => {
    const prefix = path === "/" ? "/" : path + "/";
    const names = new Map();                  // name -> filetype
    for (const p of fs.keys()) {
      if (!p.startsWith(prefix)) continue;
      const rest = p.slice(prefix.length);
      const slash = rest.indexOf("/");
      if (slash < 0) names.set(rest, FILETYPE.REGULAR_FILE);
      else names.set(rest.slice(0, slash), FILETYPE.DIRECTORY);
    }
    return [...names].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  };

  // Open files: 0-2 the console, 3 the folder "/" (preopened).
  const fds = new Map([[3, { dir: "/" }]]);
  let nextFd = 4;
  const out = { 1: [], 2: [] };
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  let instance;
  const mem = () => new DataView(instance.exports.memory.buffer);
  const bytes = () => new Uint8Array(instance.exports.memory.buffer);
  const readString = (ptr, len) => decoder.decode(bytes().slice(ptr, ptr + len));
  const argBytes = args.map((a) => encoder.encode(a + "\0"));

  const resolve = (dirFd, ptr, len) => {
    const dir = fds.get(dirFd);
    if (!dir || dir.dir === undefined) return null;
    const rel = readString(ptr, len);
    return normalize(rel.startsWith("/") ? rel : dir.dir + "/" + rel);
  };
  const grow = (file, need) => {
    if (need <= file.data.length) return;
    const bigger = new Uint8Array(Math.max(need, file.data.length * 2, 4096));
    bigger.set(file.data.subarray(0, file.size));
    file.data = bigger;
  };
  const iovecs = (ptr, count) => {
    const m = mem();
    const list = [];
    for (let i = 0; i < count; i++) list.push([m.getUint32(ptr + 8 * i, true), m.getUint32(ptr + 8 * i + 4, true)]);
    return list;
  };

  const wasi = {
    args_sizes_get(argcPtr, sizePtr) {
      const m = mem();
      m.setUint32(argcPtr, args.length, true);
      m.setUint32(sizePtr, argBytes.reduce((n, a) => n + a.length, 0), true);
      return ERRNO.SUCCESS;
    },
    args_get(argvPtr, bufPtr) {
      const m = mem();
      const b = bytes();
      argBytes.forEach((a, i) => {
        m.setUint32(argvPtr + 4 * i, bufPtr, true);
        b.set(a, bufPtr);
        bufPtr += a.length;
      });
      return ERRNO.SUCCESS;
    },
    environ_sizes_get(countPtr, sizePtr) {
      const m = mem();
      m.setUint32(countPtr, 0, true);
      m.setUint32(sizePtr, 0, true);
      return ERRNO.SUCCESS;
    },
    environ_get() { return ERRNO.SUCCESS; },
    clock_time_get(id, precision, timePtr) {
      mem().setBigUint64(timePtr, BigInt(Date.now()) * 1000000n, true);
      return ERRNO.SUCCESS;
    },
    fd_prestat_get(fd, ptr) {
      if (fd !== 3) return ERRNO.BADF;
      const m = mem();
      m.setUint8(ptr, 0);                     // a folder
      m.setUint32(ptr + 4, 1, true);          // its name: "/"
      return ERRNO.SUCCESS;
    },
    fd_prestat_dir_name(fd, ptr, len) {
      if (fd !== 3) return ERRNO.BADF;
      bytes().set(encoder.encode("/").subarray(0, len), ptr);
      return ERRNO.SUCCESS;
    },
    fd_fdstat_get(fd, ptr) {
      let type;
      if (fd <= 2) type = FILETYPE.CHARACTER_DEVICE;
      else if (!fds.has(fd)) return ERRNO.BADF;
      else type = fds.get(fd).dir !== undefined ? FILETYPE.DIRECTORY : FILETYPE.REGULAR_FILE;
      const m = mem();
      m.setUint8(ptr, type);
      m.setUint16(ptr + 2, 0, true);
      m.setBigUint64(ptr + 8, ALL_RIGHTS, true);
      m.setBigUint64(ptr + 16, ALL_RIGHTS, true);
      return ERRNO.SUCCESS;
    },
    fd_fdstat_set_flags() { return ERRNO.SUCCESS; },
    path_open(dirFd, dirFlags, pathPtr, pathLen, oflags, rightsBase, rightsInheriting, fdflags, fdPtr) {
      const path = resolve(dirFd, pathPtr, pathLen);
      if (path === null) return ERRNO.BADF;
      let open;
      if (fs.has(path)) {
        if (oflags & OFLAGS.DIRECTORY) return ERRNO.NOTDIR;
        if ((oflags & OFLAGS.CREAT) && (oflags & OFLAGS.EXCL)) return ERRNO.EXIST;
        const file = fs.get(path);
        if (oflags & OFLAGS.TRUNC) file.size = 0;
        open = { path, file, pos: 0, append: !!(fdflags & FDFLAGS_APPEND) };
      } else if (isDir(path)) {
        if (oflags & (OFLAGS.CREAT | OFLAGS.TRUNC)) return ERRNO.ISDIR;
        open = { dir: path };
      } else {
        if (!(oflags & OFLAGS.CREAT)) return ERRNO.NOENT;
        const parent = path.slice(0, path.lastIndexOf("/")) || "/";
        if (!isDir(parent)) return ERRNO.NOENT;
        const file = { data: new Uint8Array(0), size: 0 };
        fs.set(path, file);
        open = { path, file, pos: 0, append: !!(fdflags & FDFLAGS_APPEND) };
      }
      const fd = nextFd++;
      fds.set(fd, open);
      mem().setUint32(fdPtr, fd, true);
      return ERRNO.SUCCESS;
    },
    path_filestat_get(dirFd, flags, pathPtr, pathLen, ptr) {
      const path = resolve(dirFd, pathPtr, pathLen);
      if (path === null) return ERRNO.BADF;
      let type, size = 0;
      if (fs.has(path)) { type = FILETYPE.REGULAR_FILE; size = fs.get(path).size; }
      else if (isDir(path)) type = FILETYPE.DIRECTORY;
      else return ERRNO.NOENT;
      const m = mem();
      for (let i = 0; i < 64; i += 8) m.setBigUint64(ptr + i, 0n, true);
      m.setUint8(ptr + 16, type);
      m.setBigUint64(ptr + 24, 1n, true);
      m.setBigUint64(ptr + 32, BigInt(size), true);
      return ERRNO.SUCCESS;
    },
    fd_read(fd, iovsPtr, iovsLen, nreadPtr) {
      const open = fds.get(fd);
      let total = 0;
      if (open && open.file) {
        const b = bytes();
        for (const [buf, len] of iovecs(iovsPtr, iovsLen)) {
          const n = Math.max(0, Math.min(len, open.file.size - open.pos));
          b.set(open.file.data.subarray(open.pos, open.pos + n), buf);
          open.pos += n;
          total += n;
          if (n < len) break;
        }
      } else if (fd !== 0) {
        return ERRNO.BADF;                     // (the console's input: always empty)
      }
      mem().setUint32(nreadPtr, total, true);
      return ERRNO.SUCCESS;
    },
    fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
      const list = iovecs(iovsPtr, iovsLen);
      let total = 0;
      if (fd === 1 || fd === 2) {
        for (const [buf, len] of list) {
          out[fd].push(bytes().slice(buf, buf + len));
          total += len;
        }
      } else {
        const open = fds.get(fd);
        if (!open || !open.file) return ERRNO.BADF;
        const file = open.file;
        if (open.append) open.pos = file.size;
        for (const [buf, len] of list) {
          grow(file, open.pos + len);
          file.data.set(bytes().subarray(buf, buf + len), open.pos);
          open.pos += len;
          file.size = Math.max(file.size, open.pos);
          total += len;
        }
      }
      mem().setUint32(nwrittenPtr, total, true);
      return ERRNO.SUCCESS;
    },
    fd_seek(fd, offset, whence, newPtr) {
      const open = fds.get(fd);
      if (!open || !open.file) return ERRNO.BADF;
      const base = whence === 0 ? 0 : whence === 1 ? open.pos : whence === 2 ? open.file.size : NaN;
      const pos = base + Number(offset);
      if (!(pos >= 0)) return ERRNO.INVAL;
      open.pos = pos;
      mem().setBigUint64(newPtr, BigInt(pos), true);
      return ERRNO.SUCCESS;
    },
    fd_close(fd) {
      if (!fds.delete(fd) && fd > 2) return ERRNO.BADF;
      return ERRNO.SUCCESS;
    },
    fd_readdir(fd, bufPtr, bufLen, cookie, usedPtr) {
      const open = fds.get(fd);
      if (!open || open.dir === undefined) return ERRNO.BADF;
      const entries = listDir(open.dir);
      const b = bytes();
      const m = mem();
      let used = 0;
      for (let i = Number(cookie); i < entries.length && used < bufLen; i++) {
        const [name, type] = entries[i];
        const nameBytes = encoder.encode(name);
        const entry = new Uint8Array(24 + nameBytes.length);
        const v = new DataView(entry.buffer);
        v.setBigUint64(0, BigInt(i + 1), true);   // the next entry's cookie
        v.setBigUint64(8, BigInt(i + 1), true);   // an inode number
        v.setUint32(16, nameBytes.length, true);
        v.setUint8(20, type);
        entry.set(nameBytes, 24);
        const n = Math.min(entry.length, bufLen - used);   // a cut entry tells the caller to ask again
        b.set(entry.subarray(0, n), bufPtr + used);
        used += n;
      }
      m.setUint32(usedPtr, used, true);
      return ERRNO.SUCCESS;
    },
    proc_exit(code) { throw new Exit(code); },
  };
  const imports = { wasi_snapshot_preview1: new Proxy(wasi, {
    get: (target, name) => target[name] ?? (() => ERRNO.NOSYS),
  }) };

  instance = await WebAssembly.instantiate(module, imports);
  let code = 0;
  try {
    instance.exports._start();
  } catch (e) {
    if (!(e instanceof Exit)) throw e;
    code = e.code;
  }
  const text = (chunks) => decoder.decode(chunks.reduce((all, c) => {
    const joined = new Uint8Array(all.length + c.length);
    joined.set(all);
    joined.set(c, all.length);
    return joined;
  }, new Uint8Array(0)));
  const result = new Map();
  for (const [path, file] of fs) result.set(path, file.data.slice(0, file.size));
  return { code, stdout: text(out[1]), stderr: text(out[2]), files: result };
}
