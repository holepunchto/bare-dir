# bare-dir

Capability based directory access for Bare. A directory handle grants access to what's inside it and nothing else. Operations take a single name instead of a path, symbolic links are never followed, and rights can only be taken away.

```
npm i bare-dir
```

## Usage

```js
const { Directory } = require('bare-dir')

const root = await Directory.open('/srv/data')

const file = await root.openFile('notes.txt', { read: true, write: true, create: true })

await file.write(Buffer.from('hello'))

const images = (await root.openDirectory('images')).restrict(['list', 'lookup', 'read'])

for (const { name, type } of await images.list()) console.log(name, type)
```

`DirectorySync` and `FileSync` provide the same API synchronously. See [`index.d.ts`](index.d.ts) for the full API and the available rights.

## Security model

Only opening a directory by path needs ambient authority. It's meant for whoever hands out the capabilities, such as an embedder. Every other operation is relative to an open handle.

- A name is a single path component. Empty names, `.`, `..`, names with `/` or NUL, and names longer than 255 bytes fail with `EINVAL`. On Windows, so do names with `\ : " * ? < > |` or control characters, names ending in a dot or space, and device names like `CON`, `NUL.txt`, or `COM` followed by a superscript digit.
- Symbolic links are never followed. Opening one fails with `ELOOP`, and `readLink()` returns its target as text.
- Only regular files and directories can be opened. Devices, FIFOs, and sockets fail with `ENOTSUP` and are never opened, so opening them has no side effects. On Windows, the same goes for reparse points that aren't links, such as cloud file placeholders.
- A directory that is moved while open still refers to the same directory.
- Rights can only be narrowed. Anything opened from a directory gets at most its rights, and `restrict()` can only remove rights. An operation without the right it needs fails with `ENOTCAPABLE`.
- `list()` and `readLink()` fail with `EILSEQ` on names that aren't valid UTF-8, as these can't be represented as strings.

Names are checked in JavaScript and again in native code. If an invalid name reaches native code, the JavaScript checks were bypassed, and the process is aborted instead of risking an escape.

The kernel also enforces the confinement, as far as each platform allows:

| Platform | Enforcement                                                                                                                                                     |
| :------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Linux    | `openat2()` of a single name with `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS`, and `RESOLVE_NO_MAGICLINKS` from Linux 5.6, otherwise `openat()` with `O_NOFOLLOW`. |
| Android  | `openat()` of a single name with `O_NOFOLLOW`. `openat2()` is never used, as Android kills apps that call it.                                                   |
| macOS    | `openat()` of a single name with `O_NOFOLLOW_ANY`.                                                                                                              |
| iOS      | `openat()` of a single name with `O_NOFOLLOW_ANY`.                                                                                                              |
| Windows  | `NtCreateFile()` of a single name relative to the directory handle, without following reparse points. Requires Windows 10 1809 or later.                        |

## License

Apache-2.0
