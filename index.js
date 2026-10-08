const binding = require('./binding')

const rights = {
  list: binding.RIGHT_LIST,
  lookup: binding.RIGHT_LOOKUP,
  create: binding.RIGHT_CREATE,
  remove: binding.RIGHT_REMOVE,
  rename: binding.RIGHT_RENAME,
  read: binding.RIGHT_READ,
  write: binding.RIGHT_WRITE,
  append: binding.RIGHT_APPEND,
  truncate: binding.RIGHT_TRUNCATE,
  setTimes: binding.RIGHT_SET_TIMES
}

const types = {
  [binding.TYPE_FILE]: 'file',
  [binding.TYPE_DIRECTORY]: 'directory',
  [binding.TYPE_SYMLINK]: 'symlink',
  [binding.TYPE_OTHER]: 'other'
}

const handles = new WeakMap()

class Stat {
  constructor([type, size, ino, nlink, atime, mtime, ctime]) {
    this.type = types[type]
    this.size = size
    this.ino = ino
    this.nlink = nlink
    this.atime = atime
    this.mtime = mtime
    this.ctime = ctime
  }
}

class Handle {
  constructor(handle) {
    handles.set(this, handle)
  }

  get rights() {
    return namesFor(binding.rights(handleOf(this)))
  }

  restrict(rights) {
    return new this.constructor(binding.restrict(handleOf(this), maskFor(rights)))
  }
}

exports.DirectorySync = class DirectorySync extends Handle {
  static open(path, opts = {}) {
    return new DirectorySync(binding.open(validPath(path), maskFor(opts.rights), false))
  }

  openDirectory(name) {
    return new DirectorySync(binding.openDirectory(handleOf(this), validName(name), false))
  }

  openFile(name, opts) {
    return new exports.FileSync(
      binding.openFile(handleOf(this), validName(name), flagsFor(opts), false)
    )
  }

  createDirectory(name) {
    binding.createDirectory(handleOf(this), validName(name), false)
  }

  stat(name) {
    if (name === undefined) return new Stat(binding.fstat(handleOf(this), false))

    return new Stat(binding.stat(handleOf(this), validName(name), false))
  }

  list() {
    return entriesFor(binding.list(handleOf(this), false))
  }

  removeFile(name) {
    binding.remove(handleOf(this), validName(name), false, false)
  }

  removeDirectory(name) {
    binding.remove(handleOf(this), validName(name), true, false)
  }

  rename(name, directory, target = name, opts = {}) {
    const { replace = true } = opts

    binding.rename(
      handleOf(this),
      validName(name),
      handleOf(directory),
      validName(target),
      replace,
      false
    )
  }

  readLink(name) {
    return binding.readLink(handleOf(this), validName(name), false)
  }

  setTimes(atime, mtime) {
    binding.setTimes(handleOf(this), atime, mtime, false)
  }

  close() {
    binding.close(handleOf(this))
  }
}

exports.FileSync = class FileSync extends Handle {
  read(buffer, position = 0) {
    return binding.read(handleOf(this), buffer, position, false)
  }

  write(buffer, position = 0) {
    return binding.write(handleOf(this), buffer, position, false)
  }

  truncate(size) {
    binding.truncate(handleOf(this), size, false)
  }

  sync() {
    binding.sync(handleOf(this), false)
  }

  stat() {
    return new Stat(binding.fstat(handleOf(this), false))
  }

  setTimes(atime, mtime) {
    binding.setTimes(handleOf(this), atime, mtime, false)
  }

  close() {
    binding.close(handleOf(this))
  }
}

exports.Directory = class Directory extends Handle {
  static async open(path, opts = {}) {
    return new Directory(await binding.open(validPath(path), maskFor(opts.rights), true))
  }

  async openDirectory(name) {
    return new Directory(await binding.openDirectory(handleOf(this), validName(name), true))
  }

  async openFile(name, opts) {
    return new exports.File(
      await binding.openFile(handleOf(this), validName(name), flagsFor(opts), true)
    )
  }

  async createDirectory(name) {
    await binding.createDirectory(handleOf(this), validName(name), true)
  }

  async stat(name) {
    if (name === undefined) return new Stat(await binding.fstat(handleOf(this), true))

    return new Stat(await binding.stat(handleOf(this), validName(name), true))
  }

  async list() {
    return entriesFor(await binding.list(handleOf(this), true))
  }

  async removeFile(name) {
    await binding.remove(handleOf(this), validName(name), false, true)
  }

  async removeDirectory(name) {
    await binding.remove(handleOf(this), validName(name), true, true)
  }

  async rename(name, directory, target = name, opts = {}) {
    const { replace = true } = opts

    await binding.rename(
      handleOf(this),
      validName(name),
      handleOf(directory),
      validName(target),
      replace,
      true
    )
  }

  async readLink(name) {
    return await binding.readLink(handleOf(this), validName(name), true)
  }

  async setTimes(atime, mtime) {
    await binding.setTimes(handleOf(this), atime, mtime, true)
  }

  close() {
    binding.close(handleOf(this))
  }
}

exports.File = class File extends Handle {
  async read(buffer, position = 0) {
    return await binding.read(handleOf(this), buffer, position, true)
  }

  async write(buffer, position = 0) {
    return await binding.write(handleOf(this), buffer, position, true)
  }

  async truncate(size) {
    await binding.truncate(handleOf(this), size, true)
  }

  async sync() {
    await binding.sync(handleOf(this), true)
  }

  async stat() {
    return new Stat(await binding.fstat(handleOf(this), true))
  }

  async setTimes(atime, mtime) {
    await binding.setTimes(handleOf(this), atime, mtime, true)
  }

  close() {
    binding.close(handleOf(this))
  }
}

exports.rights = Object.freeze(Object.keys(rights))

for (const constructor of [
  Stat,
  Handle,
  exports.DirectorySync,
  exports.FileSync,
  exports.Directory,
  exports.File
]) {
  Object.freeze(constructor)
  Object.freeze(constructor.prototype)
}

Object.freeze(exports)

function handleOf(object) {
  const handle = handles.get(object)

  if (handle === undefined) {
    throw new TypeError('Expected a directory or file instance')
  }

  return handle
}

function validPath(path) {
  if (typeof path !== 'string') {
    throw new TypeError(`Path must be a string. Received type ${typeof path} (${path})`)
  }

  return path
}

function validName(name) {
  if (typeof name !== 'string' || !binding.validName(name)) {
    throw invalid(`Name '${name}' is not a single, valid name`)
  }

  return name
}

function flagsFor(opts = {}) {
  const {
    write = false,
    append = false,
    read = !write && !append,
    create = false,
    exclusive = false,
    truncate = false
  } = opts

  if (exclusive && !create) throw invalid('exclusive requires create')
  if (truncate && !write) throw invalid('truncate requires write')

  let flags = 0

  if (read) flags |= binding.OPEN_READ
  if (write) flags |= binding.OPEN_WRITE
  if (append) flags |= binding.OPEN_APPEND
  if (create) flags |= binding.OPEN_CREATE
  if (exclusive) flags |= binding.OPEN_EXCLUSIVE
  if (truncate) flags |= binding.OPEN_TRUNCATE

  return flags
}

function entriesFor(entries) {
  return entries.map(([name, type]) => ({ name, type: types[type] }))
}

function maskFor(names = exports.rights) {
  let mask = 0

  for (const name of names) {
    if (Object.hasOwn(rights, name) === false) {
      throw new TypeError(`Unknown right '${name}'`)
    }

    mask |= rights[name]
  }

  return mask
}

function namesFor(mask) {
  return exports.rights.filter((name) => (mask & rights[name]) !== 0)
}

function invalid(msg) {
  const err = new Error(`EINVAL: ${msg}`)

  err.code = 'EINVAL'

  return err
}
