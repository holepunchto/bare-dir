/**
 * A right of a directory or file handle:
 *
 * - `list`: list the entries of a directory.
 * - `lookup`: open entries, get their metadata, and read symbolic links.
 * - `create`: create files and directories.
 * - `remove`: remove entries, including replacing one by renaming.
 * - `rename`: move entries between directories. Both directories need it.
 * - `read`: read files.
 * - `write`: write files anywhere.
 * - `append`: write files only at their end.
 * - `truncate`: truncate files.
 * - `setTimes`: set the access and modification times.
 *
 * Operations without the right they need fail with `ENOTCAPABLE`.
 */
type Right =
  | 'list'
  | 'lookup'
  | 'create'
  | 'remove'
  | 'rename'
  | 'read'
  | 'write'
  | 'append'
  | 'truncate'
  | 'setTimes'

/** The metadata of an entry. Times are in nanoseconds since the Unix epoch. */
interface Stat {
  /** The type of the entry. Symbolic links are never followed. */
  type: 'file' | 'directory' | 'symlink' | 'other'
  size: bigint
  ino: bigint
  nlink: bigint
  atime: bigint
  mtime: bigint
  ctime: bigint
}

/** An entry of a directory. */
interface Entry {
  name: string
  type: Stat['type']
}

interface OpenOptions {
  /** The rights of the directory. Defaults to all of them. */
  rights?: Right[]
}

interface OpenFileOptions {
  /** Open the file for reading. Defaults to `true` unless writing or appending. */
  read?: boolean
  /** Open the file for writing anywhere. */
  write?: boolean
  /** Open the file for writing only at its end. */
  append?: boolean
  /** Create the file if it doesn't exist. */
  create?: boolean
  /** Fail with `EEXIST` if the file exists. Requires `create`. */
  exclusive?: boolean
  /** Truncate the file to empty. Requires `write`. */
  truncate?: boolean
}

interface RenameOptions {
  /**
   * Replace an existing target, which needs `remove` on the target directory.
   * Otherwise, an existing target fails with `EEXIST`. Defaults to `true`.
   */
  replace?: boolean
}

/**
 * A directory capability, whose operations run on the thread pool and return
 * promises. Names are single path components; anything else fails with
 * `EINVAL`. Names and rights are checked before an operation starts.
 */
declare class Directory {
  /**
   * Open the directory at `path`. This is the only operation that takes a path,
   * and is meant for whoever hands out the capabilities.
   */
  static open(path: string, opts?: OpenOptions): Promise<Directory>

  /** The rights of the directory. */
  readonly rights: Right[]

  /** Open the directory `name` with the rights of this one. Needs `lookup`. */
  openDirectory(name: string): Promise<Directory>

  /**
   * Open the regular file `name`. Needs `lookup`, and the right of the same
   * name for `read`, `write`, `append`, `create` and `truncate`. The file gets
   * the rights it was opened for, plus `setTimes`, and `truncate` if opened for
   * writing, where this directory has them. Symbolic links fail with `ELOOP`,
   * and anything that isn't a regular file with `EISDIR` or `ENOTSUP`.
   */
  openFile(name: string, opts?: OpenFileOptions): Promise<File>

  /** Create the directory `name`. Needs `create`. */
  createDirectory(name: string): Promise<void>

  /**
   * Get the metadata of the entry `name`, or of the directory itself if `name`
   * is omitted. Needs `lookup` for an entry.
   */
  stat(name?: string): Promise<Stat>

  /**
   * List the entries of the directory. Needs `list`. Fails with `EILSEQ` if a
   * name isn't valid UTF-8.
   */
  list(): Promise<Entry[]>

  /** Remove the entry `name`, which must not be a directory. Needs `remove`. */
  removeFile(name: string): Promise<void>

  /** Remove the empty directory `name`. Needs `remove`. */
  removeDirectory(name: string): Promise<void>

  /**
   * Move the entry `name` to `target`, defaulting to `name`, in `directory`,
   * which may be this one. Needs `rename` on both directories.
   */
  rename(
    name: string,
    directory: Directory | DirectorySync,
    target?: string,
    opts?: RenameOptions
  ): Promise<void>

  /**
   * Read the target of the symbolic link `name` as text. Needs `lookup`. Fails
   * with `EILSEQ` if the target isn't valid UTF-8.
   */
  readLink(name: string): Promise<string>

  /**
   * Set the access and modification times of the directory, leaving either
   * unchanged if `null`. Needs `setTimes`.
   */
  setTimes(atime: bigint | null, mtime: bigint | null): Promise<void>

  /** Get a new handle to the directory with only those `rights` that this one has. */
  restrict(rights: Right[]): Directory

  /**
   * Close the directory. Operations already in progress finish first, and
   * later ones fail with `EBADF`. Handles are also closed when garbage
   * collected.
   */
  close(): void
}

/** A file capability, whose operations run on the thread pool and return promises. */
declare class File {
  /** The rights of the file. */
  readonly rights: Right[]

  /**
   * Read into `buffer` from `position`, defaulting to `0`, and resolve with the
   * number of bytes read. Needs `read`.
   */
  read(buffer: Uint8Array, position?: number): Promise<number>

  /**
   * Write `buffer` at `position`, defaulting to `0`, and resolve with the
   * number of bytes written. A file opened only for appending always writes at
   * its end. Needs `write` or `append`.
   */
  write(buffer: Uint8Array, position?: number): Promise<number>

  /** Truncate the file to `size` bytes. Needs `truncate`. */
  truncate(size: number): Promise<void>

  /** Flush the file to storage. */
  sync(): Promise<void>

  /** Get the metadata of the file. */
  stat(): Promise<Stat>

  /**
   * Set the access and modification times of the file, leaving either
   * unchanged if `null`. Needs `setTimes`.
   */
  setTimes(atime: bigint | null, mtime: bigint | null): Promise<void>

  /** Get a new handle to the file with only those `rights` that this one has. */
  restrict(rights: Right[]): File

  /**
   * Close the file. Operations already in progress finish first, and later ones
   * fail with `EBADF`. Handles are also closed when garbage collected.
   */
  close(): void
}

/** The synchronous counterpart of `Directory`. */
declare class DirectorySync {
  /**
   * Open the directory at `path`. This is the only operation that takes a path,
   * and is meant for whoever hands out the capabilities.
   */
  static open(path: string, opts?: OpenOptions): DirectorySync

  /** The rights of the directory. */
  readonly rights: Right[]

  /** Open the directory `name` with the rights of this one. Needs `lookup`. */
  openDirectory(name: string): DirectorySync

  /**
   * Open the regular file `name`. Needs `lookup`, and the right of the same
   * name for `read`, `write`, `append`, `create` and `truncate`. The file gets
   * the rights it was opened for, plus `setTimes`, and `truncate` if opened for
   * writing, where this directory has them. Symbolic links fail with `ELOOP`,
   * and anything that isn't a regular file with `EISDIR` or `ENOTSUP`.
   */
  openFile(name: string, opts?: OpenFileOptions): FileSync

  /** Create the directory `name`. Needs `create`. */
  createDirectory(name: string): void

  /**
   * Get the metadata of the entry `name`, or of the directory itself if `name`
   * is omitted. Needs `lookup` for an entry.
   */
  stat(name?: string): Stat

  /**
   * List the entries of the directory. Needs `list`. Fails with `EILSEQ` if a
   * name isn't valid UTF-8.
   */
  list(): Entry[]

  /** Remove the entry `name`, which must not be a directory. Needs `remove`. */
  removeFile(name: string): void

  /** Remove the empty directory `name`. Needs `remove`. */
  removeDirectory(name: string): void

  /**
   * Move the entry `name` to `target`, defaulting to `name`, in `directory`,
   * which may be this one. Needs `rename` on both directories.
   */
  rename(
    name: string,
    directory: Directory | DirectorySync,
    target?: string,
    opts?: RenameOptions
  ): void

  /**
   * Read the target of the symbolic link `name` as text. Needs `lookup`. Fails
   * with `EILSEQ` if the target isn't valid UTF-8.
   */
  readLink(name: string): string

  /**
   * Set the access and modification times of the directory, leaving either
   * unchanged if `null`. Needs `setTimes`.
   */
  setTimes(atime: bigint | null, mtime: bigint | null): void

  /** Get a new handle to the directory with only those `rights` that this one has. */
  restrict(rights: Right[]): DirectorySync

  /** Close the directory. Handles are also closed when garbage collected. */
  close(): void
}

/** The synchronous counterpart of `File`. */
declare class FileSync {
  /** The rights of the file. */
  readonly rights: Right[]

  /**
   * Read into `buffer` from `position`, defaulting to `0`, and return the
   * number of bytes read. Needs `read`.
   */
  read(buffer: Uint8Array, position?: number): number

  /**
   * Write `buffer` at `position`, defaulting to `0`, and return the number of
   * bytes written. A file opened only for appending always writes at its end.
   * Needs `write` or `append`.
   */
  write(buffer: Uint8Array, position?: number): number

  /** Truncate the file to `size` bytes. Needs `truncate`. */
  truncate(size: number): void

  /** Flush the file to storage. */
  sync(): void

  /** Get the metadata of the file. */
  stat(): Stat

  /**
   * Set the access and modification times of the file, leaving either
   * unchanged if `null`. Needs `setTimes`.
   */
  setTimes(atime: bigint | null, mtime: bigint | null): void

  /** Get a new handle to the file with only those `rights` that this one has. */
  restrict(rights: Right[]): FileSync

  /** Close the file. Handles are also closed when garbage collected. */
  close(): void
}

/** The names of all rights. */
declare const rights: Right[]

export { Directory, DirectorySync, File, FileSync, rights, type Right, type Stat, type Entry }
