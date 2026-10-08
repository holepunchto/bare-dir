const test = require('brittle')
const fs = require('bare-fs')
const os = require('bare-os')
const path = require('bare-path')
const { spawnSync } = require('bare-subprocess')
const { Directory, DirectorySync, FileSync } = require('.')

const isWindows = Bare.platform === 'win32'

test('read a file', (t) => {
  const { root } = fixture(t)

  const file = root.openFile('file.txt')

  const buffer = Buffer.alloc(16)
  const n = file.read(buffer)

  t.is(buffer.subarray(0, n).toString(), 'hello')

  file.close()
})

test('create, write and read back a file', (t) => {
  const { root } = fixture(t)

  const file = root.openFile('new.txt', { read: true, write: true, create: true, exclusive: true })

  t.is(file.write(Buffer.from('world')), 5)

  const buffer = Buffer.alloc(16)
  const n = file.read(buffer)

  t.is(buffer.subarray(0, n).toString(), 'world')

  file.truncate(2)

  t.is(file.stat().size, 2n)

  file.close()

  t.exception(() => root.openFile('new.txt', { write: true, create: true, exclusive: true }), {
    code: 'EEXIST'
  })
})

test('list and stat entries without following symbolic links', (t) => {
  const { root } = fixture(t)

  const entries = Object.fromEntries(root.list().map(({ name, type }) => [name, type]))

  t.is(entries['file.txt'], 'file')
  t.is(entries.sub, 'directory')
  t.is(entries['link-outside'], 'symlink')

  t.is(root.stat('file.txt').type, 'file')
  t.is(root.stat('link-outside').type, 'symlink')
  t.is(root.stat().type, 'directory')
})

test('create, rename and remove entries', (t) => {
  const { root } = fixture(t)

  root.createDirectory('made')

  const sub = root.openDirectory('sub')

  root.rename('file.txt', sub, 'moved.txt')

  t.is(sub.stat('moved.txt').type, 'file')
  t.exception(() => root.stat('file.txt'), { code: 'ENOENT' })

  sub.rename('moved.txt', root, 'file.txt', { replace: false })

  t.exception(() => sub.rename('inner.txt', root, 'file.txt', { replace: false }), {
    code: 'EEXIST'
  })

  root.removeFile('file.txt')
  root.removeDirectory('made')

  t.exception(() => root.removeDirectory('sub'), { code: 'ENOTEMPTY' })
})

test('read a symbolic link as text', (t) => {
  const { root } = fixture(t)

  const target = root.readLink('link-file')

  // Windows stores the target as an absolute path.
  if (isWindows) t.ok(target.endsWith('\\etc\\hosts'), target)
  else t.is(target, '/etc/hosts')
})

test('set times', (t) => {
  const { root } = fixture(t)

  const file = root.openFile('file.txt', { write: true })

  file.setTimes(null, 1000000000n)

  t.is(file.stat().mtime, 1000000000n)

  file.setTimes(-1500000000n, null)

  t.is(file.stat().atime, -1500000000n)

  file.close()
})

test('names that are not a single name are rejected', (t) => {
  const { root } = fixture(t)

  const names = [
    '',
    '.',
    '..',
    '../outside',
    '../outside/secret.txt',
    '/etc/hosts',
    'sub/inner.txt',
    'sub/../file.txt',
    'file.txt\0.png',
    'x'.repeat(256),
    42,
    null
  ]

  if (isWindows) {
    names.push(
      'sub\\inner.txt',
      'C:',
      'file.txt:stream',
      'CON',
      'nul.txt',
      'COM\u00b9',
      'file.txt.'
    )
  }

  for (const name of names) {
    t.exception(() => root.openFile(name), { code: 'EINVAL' }, `openFile(${JSON.stringify(name)})`)
    t.exception(
      () => root.openDirectory(name),
      { code: 'EINVAL' },
      `openDirectory(${JSON.stringify(name)})`
    )
    t.exception(() => root.stat(name), { code: 'EINVAL' }, `stat(${JSON.stringify(name)})`)
    t.exception(
      () => root.rename('file.txt', root, name),
      { code: 'EINVAL' },
      `rename(${JSON.stringify(name)})`
    )
  }
})

test('symbolic links are never followed', (t) => {
  const { root } = fixture(t)

  t.exception(() => root.openFile('link-file'), { code: 'ELOOP' })
  t.exception(() => root.openFile('link-outside'), { code: 'ELOOP' })
  t.exception(() => root.openDirectory('link-outside'), { code: 'ELOOP' })
  t.exception(() => root.openDirectory('link-inside'), { code: 'ELOOP' })
  t.exception(() => root.openFile('link-file', { write: true, create: true }), { code: 'ELOOP' })
})

test('special files are never opened', { skip: isWindows }, (t) => {
  const { root } = fixture(t)

  t.exception(() => root.openFile('fifo'), { code: 'ENOTSUP' })
  t.exception(() => root.openFile('fifo', { write: true }), { code: 'ENOTSUP' })
})

test('a directory stays confined after it is moved outside the root', (t) => {
  const { root, dir } = fixture(t)

  const sub = root.openDirectory('sub')

  fs.renameSync(path.join(dir, 'root', 'sub'), path.join(dir, 'outside', 'moved'))

  t.alike(
    sub.list().map(({ name }) => name),
    ['inner.txt']
  )

  t.exception(() => sub.openDirectory('..'), { code: 'EINVAL' })
  t.exception(() => root.stat('sub'), { code: 'ENOENT' })
})

test('rights are enforced', (t) => {
  const { root } = fixture(t)

  const readOnly = root.restrict(['list', 'lookup', 'read'])

  t.alike(readOnly.rights, ['list', 'lookup', 'read'])

  t.exception(() => readOnly.openFile('file.txt', { write: true }), { code: 'ENOTCAPABLE' })
  t.exception(() => readOnly.openFile('new.txt', { read: true, create: true }), {
    code: 'ENOTCAPABLE'
  })
  t.exception(() => readOnly.createDirectory('made'), { code: 'ENOTCAPABLE' })
  t.exception(() => readOnly.removeFile('file.txt'), { code: 'ENOTCAPABLE' })
  t.exception(() => readOnly.rename('file.txt', readOnly, 'other.txt'), { code: 'ENOTCAPABLE' })
  t.exception(() => readOnly.setTimes(null, 0n), { code: 'ENOTCAPABLE' })

  const file = readOnly.openFile('file.txt')

  t.alike(file.rights, ['read'])
  t.exception(() => file.write(Buffer.from('x')), { code: 'ENOTCAPABLE' })
  t.exception(() => file.truncate(0), { code: 'ENOTCAPABLE' })

  file.close()

  const blind = root.restrict(['lookup', 'read'])

  t.exception(() => blind.list(), { code: 'ENOTCAPABLE' })
})

test('classes and exports are frozen', (t) => {
  const { root } = fixture(t)

  const exports = require('.')

  t.ok(Object.isFrozen(exports))
  t.ok(Object.isFrozen(exports.rights))

  const file = root.openFile('file.txt')

  for (const object of [root, file, root.stat()]) {
    let prototype = Object.getPrototypeOf(object)

    while (prototype !== Object.prototype) {
      t.ok(Object.isFrozen(prototype))
      t.ok(Object.isFrozen(prototype.constructor))

      prototype = Object.getPrototypeOf(prototype)
    }
  }

  file.close()
})

test('rights are only ever narrowed', (t) => {
  const { root } = fixture(t)

  const readOnly = root.restrict(['lookup', 'read'])

  t.alike(readOnly.restrict(['lookup', 'read', 'write', 'create']).rights, ['lookup', 'read'])
  t.alike(readOnly.openDirectory('sub').rights, ['lookup', 'read'])

  t.exception.all(() => root.restrict(['everything']), TypeError)
})

test('renaming needs rights on both directories', (t) => {
  const { root } = fixture(t)

  const sub = root.openDirectory('sub').restrict(['lookup', 'read'])

  t.exception(() => root.rename('file.txt', sub, 'file.txt'), { code: 'ENOTCAPABLE' })
  t.exception(() => sub.rename('inner.txt', root, 'inner.txt'), { code: 'ENOTCAPABLE' })

  const noRemove = root.restrict(['lookup', 'rename'])

  t.exception(() => root.rename('file.txt', noRemove, 'other.txt'), { code: 'ENOTCAPABLE' })

  root.rename('file.txt', noRemove, 'other.txt', { replace: false })

  t.is(root.stat('other.txt').type, 'file')
})

test('an append only file always writes at its end', (t) => {
  const { root } = fixture(t)

  const appendOnly = root.restrict(['lookup', 'append'])

  t.exception(() => appendOnly.openFile('file.txt', { write: true }), { code: 'ENOTCAPABLE' })

  const file = appendOnly.openFile('file.txt', { append: true })

  file.write(Buffer.from(' world'), 0)
  file.close()

  const check = root.openFile('file.txt')
  const buffer = Buffer.alloc(32)

  t.is(buffer.subarray(0, check.read(buffer)).toString(), 'hello world')

  check.close()
})

test('closed handles and handles of the wrong kind are refused', (t) => {
  const { root } = fixture(t)

  const file = root.openFile('file.txt')

  t.exception.all(() => root.rename('file.txt', file, 'x'), TypeError)
  t.exception.all(() => root.rename('file.txt', {}, 'x'), TypeError)
  t.exception.all(() => new DirectorySync({}).list(), TypeError)
  t.exception.all(() => new FileSync(Object.create(null)).read(Buffer.alloc(1)), TypeError)

  for (const value of [42, 'handle', true, null, Symbol('handle'), 1n, () => {}]) {
    t.exception.all(() => new DirectorySync(value).list(), TypeError)
    t.exception.all(() => new DirectorySync(value).rights, TypeError)
  }

  file.close()

  t.exception(() => file.read(Buffer.alloc(1)), { code: 'EBADF' })
})

test('names that are not valid UTF-8 fail listing', { skip: isWindows }, (t) => {
  const { root, dir } = fixture(t)

  const created = spawnSync('sh', ['-c', 'touch "$(printf \'\\377\')"'], {
    cwd: path.join(dir, 'root')
  })

  if (created.status !== 0) {
    t.comment('The file system refuses names that are not valid UTF-8')

    return
  }

  t.exception(() => root.list(), { code: 'EILSEQ' })

  // bare-fs can't remove it either.
  spawnSync('sh', ['-c', 'rm "$(printf \'\\377\')"'], { cwd: path.join(dir, 'root') })
})

test('link targets that are not valid UTF-8 fail reading', { skip: isWindows }, (t) => {
  const { root, dir } = fixture(t)

  spawnSync('sh', ['-c', 'ln -s "$(printf \'\\377\')" invalid'], { cwd: path.join(dir, 'root') })

  t.exception(() => root.readLink('invalid'), { code: 'EILSEQ' })
})

test('an invalid name that reaches the native layer stops the process', (t) => {
  const { dir } = fixture(t)

  const binding = path.join(__dirname, 'binding.js')
  const root = path.join(dir, 'root')

  const source = `
    const binding = require(${JSON.stringify(binding)})
    const root = binding.open(${JSON.stringify(root)}, binding.RIGHT_LOOKUP, false)
    binding.stat(root, '..', false)
  `

  const result = spawnSync(os.execPath(), ['-e', source])

  // Aborting exits with STATUS_STACK_BUFFER_OVERRUN on Windows.
  if (isWindows) t.is(result.status, 0xc0000409)
  else t.is(result.signal, 'SIGABRT')
  t.ok(result.stderr.toString().includes('bare-dir: an invalid name reached the native layer'))
})

test('exiting with operations in flight', (t) => {
  const { dir } = fixture(t)

  const index = path.join(__dirname, 'index.js')
  const root = path.join(dir, 'root')

  const source = `
    const { Directory } = require(${JSON.stringify(index)})
    Directory.open(${JSON.stringify(root)}).then(async (root) => {
      const file = await root.openFile('file.txt')
      const settled = () => console.log('settled')
      for (let i = 0; i < 64; i++) {
        root.openFile('file.txt').then(settled, settled)
        root.list().then(settled, settled)
        file.read(Buffer.alloc(16)).then(settled, settled)
      }
      file.close()
      root.close()
      Bare.exit(0)
    })
  `

  const result = spawnSync(os.execPath(), ['-e', source])

  t.is(result.status, 0, result.stderr.toString())
  t.is(result.stdout.toString(), '')
})

test('async: read, write and list', async (t) => {
  const { dir } = fixture(t)

  const root = await Directory.open(path.join(dir, 'root'))

  const file = await root.openFile('new.txt', { read: true, write: true, create: true })

  t.is(await file.write(Buffer.from('async')), 5)

  const buffer = Buffer.alloc(16)

  t.is(buffer.subarray(0, await file.read(buffer)).toString(), 'async')
  t.is((await file.stat()).size, 5n)

  file.close()

  const names = (await root.list()).map(({ name }) => name)

  t.ok(names.includes('new.txt'))
  t.is((await root.stat('sub')).type, 'directory')

  root.close()
})

test('async: create, rename and remove entries', async (t) => {
  const { dir } = fixture(t)

  const root = await Directory.open(path.join(dir, 'root'))
  const sub = await root.openDirectory('sub')

  await root.createDirectory('made')
  await root.rename('file.txt', sub, 'moved.txt')

  t.is((await sub.stat('moved.txt')).type, 'file')

  await sub.removeFile('moved.txt')
  await root.removeDirectory('made')

  await t.exception(root.stat('made'), { code: 'ENOENT' })

  sub.close()
  root.close()
})

test('async: failures reject rather than throw', async (t) => {
  const { dir } = fixture(t)

  const root = await Directory.open(path.join(dir, 'root'))

  const invalid = root.openFile('../outside/secret.txt')

  t.ok(invalid instanceof Promise)

  await t.exception(invalid, { code: 'EINVAL' })
  await t.exception(root.openFile('link-outside'), { code: 'ELOOP' })
  await t.exception(root.restrict(['lookup']).openFile('file.txt'), { code: 'ENOTCAPABLE' })
  await t.exception(Directory.open(path.join(dir, 'missing')), { code: 'ENOENT' })

  root.close()
})

test('async: many operations in flight at once', async (t) => {
  const { dir } = fixture(t)

  const root = await Directory.open(path.join(dir, 'root'))
  const file = await root.openFile('file.txt')

  const reads = []

  for (let i = 0; i < 64; i++) {
    const buffer = Buffer.alloc(5)

    reads.push(file.read(buffer).then((n) => buffer.subarray(0, n).toString()))
  }

  const stats = []

  for (let i = 0; i < 64; i++) stats.push(root.stat('file.txt'))

  t.alike(await Promise.all(reads), new Array(64).fill('hello'))
  t.ok((await Promise.all(stats)).every((stat) => stat.type === 'file'))

  file.close()
  root.close()
})

test('async: operations in flight finish after close', async (t) => {
  const { dir } = fixture(t)

  const root = await Directory.open(path.join(dir, 'root'))
  const file = await root.openFile('file.txt')

  const buffer = Buffer.alloc(5)
  const read = file.read(buffer)

  file.close()

  t.is(await read, 5)
  t.is(buffer.toString(), 'hello')

  await t.exception(file.read(Buffer.alloc(5)), { code: 'EBADF' })

  root.close()
})

test('async: buffers detached while in flight are kept alive', async (t) => {
  const { dir } = fixture(t)

  const root = await Directory.open(path.join(dir, 'root'))
  const file = await root.openFile('new.txt', { read: true, write: true, create: true })

  const size = 1 << 20

  const written = Buffer.alloc(size, 0x42)
  const write = file.write(written)
  written.buffer.transfer(0)
  Buffer.alloc(size, 0x43)

  t.is(await write, size)

  const read = Buffer.alloc(size)
  const pending = file.read(read)
  read.buffer.transfer(0)
  Buffer.alloc(size, 0x43)

  t.is(await pending, size)

  const buffer = Buffer.alloc(size)
  await file.read(buffer)

  t.ok(buffer.every((byte) => byte === 0x42))

  file.close()
  root.close()
})

test('async: views read and write at their offset', async (t) => {
  const { dir } = fixture(t)

  const root = await Directory.open(path.join(dir, 'root'))
  const file = await root.openFile('new.txt', { read: true, write: true, create: true })

  await file.write(new Uint8Array([1, 2, 3, 4, 5, 6]).subarray(2, 4))

  const shared = new Uint8Array(new SharedArrayBuffer(8))

  t.is(await file.read(shared.subarray(4)), 2)
  t.alike([...shared], [0, 0, 0, 0, 3, 4, 0, 0])

  file.close()
  root.close()
})

test('async: rename between asynchronous and synchronous directories', async (t) => {
  const { dir, root } = fixture(t)

  const sub = await Directory.open(path.join(dir, 'root', 'sub'))

  await sub.rename('inner.txt', root, 'inner.txt')

  t.is(root.stat('inner.txt').type, 'file')

  sub.close()
})

// A root to open, an outside to escape to, and links in the root that point
// out of it.
function fixture(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bare-dir-'))

  t.teardown(() => fs.rmSync(dir, { recursive: true, force: true }))

  fs.mkdirSync(path.join(dir, 'outside'))
  fs.writeFileSync(path.join(dir, 'outside', 'secret.txt'), 'secret')

  fs.mkdirSync(path.join(dir, 'root'))
  fs.writeFileSync(path.join(dir, 'root', 'file.txt'), 'hello')
  fs.mkdirSync(path.join(dir, 'root', 'sub'))
  fs.writeFileSync(path.join(dir, 'root', 'sub', 'inner.txt'), 'inner')

  fs.symlinkSync(path.join(dir, 'outside'), path.join(dir, 'root', 'link-outside'))
  fs.symlinkSync('/etc/hosts', path.join(dir, 'root', 'link-file'))
  fs.symlinkSync('sub', path.join(dir, 'root', 'link-inside'))

  if (!isWindows) spawnSync('mkfifo', [path.join(dir, 'root', 'fifo')])

  const root = DirectorySync.open(path.join(dir, 'root'))

  t.teardown(() => root.close())

  return { dir, root }
}
