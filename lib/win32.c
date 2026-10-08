#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>

#include "dir.h"

// Entries are only opened by a single name relative to their directory handle.
// A Win32 path would bring back device names and other DOS path rules.

#ifndef OBJ_DONT_REPARSE
#define OBJ_DONT_REPARSE 0x00001000
#endif

#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS) (status)) >= 0)
#endif

#define BARE_DIR_STATUS_INVALID_INFO_CLASS        ((NTSTATUS) 0xC0000003)
#define BARE_DIR_STATUS_ACCESS_DENIED             ((NTSTATUS) 0xC0000022)
#define BARE_DIR_STATUS_INVALID_PARAMETER         ((NTSTATUS) 0xC000000D)
#define BARE_DIR_STATUS_OBJECT_NAME_INVALID       ((NTSTATUS) 0xC0000033)
#define BARE_DIR_STATUS_FILE_IS_A_DIRECTORY       ((NTSTATUS) 0xC00000BA)
#define BARE_DIR_STATUS_NOT_SUPPORTED             ((NTSTATUS) 0xC00000BB)
#define BARE_DIR_STATUS_NOT_A_DIRECTORY           ((NTSTATUS) 0xC0000103)
#define BARE_DIR_STATUS_REPARSE_POINT_ENCOUNTERED ((NTSTATUS) 0xC000050B)
#define BARE_DIR_STATUS_STOPPED_ON_SYMLINK        ((NTSTATUS) 0x8000002D)

#define BARE_DIR_FILE_RENAME_INFORMATION         10
#define BARE_DIR_FILE_DISPOSITION_INFORMATION    13
#define BARE_DIR_FILE_DISPOSITION_INFORMATION_EX 64
#define BARE_DIR_FILE_RENAME_INFORMATION_EX      65

#define BARE_DIR_FILE_DISPOSITION_DELETE                    0x01
#define BARE_DIR_FILE_DISPOSITION_POSIX_SEMANTICS           0x02
#define BARE_DIR_FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE 0x10

#define BARE_DIR_FILE_RENAME_REPLACE_IF_EXISTS         0x01
#define BARE_DIR_FILE_RENAME_POSIX_SEMANTICS           0x02
#define BARE_DIR_FILE_RENAME_IGNORE_READONLY_ATTRIBUTE 0x40

// The Unix epoch in 100 nanosecond intervals since 1601.
#define BARE_DIR_EPOCH_OFFSET 116444736000000000LL

NTSYSAPI NTSTATUS NTAPI
NtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID info, ULONG len, FILE_INFORMATION_CLASS info_class);

typedef struct {
  ULONG Flags;
} bare_dir_disposition_ex_t;

typedef struct {
  BOOLEAN DeleteFile;
} bare_dir_disposition_t;

typedef struct {
  union {
    BOOLEAN ReplaceIfExists;
    ULONG Flags;
  };
  HANDLE RootDirectory;
  ULONG FileNameLength;
  WCHAR FileName[1];
} bare_dir_rename_t;

typedef struct {
  ULONG ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  union {
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      ULONG Flags;
      WCHAR PathBuffer[1];
    } SymbolicLinkReparseBuffer;
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      WCHAR PathBuffer[1];
    } MountPointReparseBuffer;
  };
} bare_dir_reparse_data_t;

static int
bare_dir__error(void) {
  return uv_translate_sys_error(GetLastError());
}

static int
bare_dir__status(NTSTATUS status) {
  switch (status) {
  case BARE_DIR_STATUS_FILE_IS_A_DIRECTORY:
    return UV_EISDIR;
  case BARE_DIR_STATUS_NOT_A_DIRECTORY:
    return UV_ENOTDIR;
  case BARE_DIR_STATUS_REPARSE_POINT_ENCOUNTERED:
  case BARE_DIR_STATUS_STOPPED_ON_SYMLINK:
    return UV_ELOOP;
  case BARE_DIR_STATUS_OBJECT_NAME_INVALID:
    return UV_EINVAL;
  }

  return uv_translate_sys_error(RtlNtStatusToDosError(status));
}

static bool
bare_dir__unsupported(NTSTATUS status) {
  return status == BARE_DIR_STATUS_INVALID_PARAMETER || status == BARE_DIR_STATUS_NOT_SUPPORTED || status == BARE_DIR_STATUS_INVALID_INFO_CLASS;
}

// A name of at most 255 bytes is at most 255 UTF-16 code units.
static int
bare_dir__to_wide(const char *name, WCHAR wide[256], USHORT *len) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, (int) strlen(name), wide, 256);

  if (n <= 0) return UV_EINVAL;

  *len = (USHORT) n;

  return 0;
}

static NTSTATUS
bare_dir__open_relative(HANDLE dir, const WCHAR *name, USHORT len, ACCESS_MASK access, ULONG disposition, ULONG options, HANDLE *result) {
  UNICODE_STRING object_name = {
    .Length = len * sizeof(WCHAR),
    .MaximumLength = len * sizeof(WCHAR),
    .Buffer = (PWSTR) name,
  };

  OBJECT_ATTRIBUTES attributes;
  InitializeObjectAttributes(&attributes, &object_name, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, dir, NULL);

  IO_STATUS_BLOCK io;

  return NtCreateFile(
    result,
    access | SYNCHRONIZE,
    &attributes,
    &io,
    NULL,
    FILE_ATTRIBUTE_NORMAL,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    disposition,
    options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
    NULL,
    0
  );
}

static int
bare_dir__open(HANDLE dir, const char *name, ACCESS_MASK access, ULONG disposition, ULONG options, HANDLE *result) {
  int err;

  WCHAR wide[256];
  USHORT len;

  err = bare_dir__to_wide(name, wide, &len);
  if (err < 0) return err;

  // Write access to the attributes is only needed to set times, so it's dropped
  // where it's denied.
  NTSTATUS status = bare_dir__open_relative(dir, wide, len, access | FILE_WRITE_ATTRIBUTES, disposition, options, result);

  if (status == BARE_DIR_STATUS_ACCESS_DENIED) {
    status = bare_dir__open_relative(dir, wide, len, access, disposition, options, result);
  }

  if (!NT_SUCCESS(status)) return bare_dir__status(status);

  return 0;
}

static int
bare_dir__type(DWORD attributes, DWORD tag) {
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return IsReparseTagNameSurrogate(tag) ? bare_dir_type_symlink : bare_dir_type_other;
  }

  if (attributes & FILE_ATTRIBUTE_DIRECTORY) return bare_dir_type_directory;

  return bare_dir_type_file;
}

static int64_t
bare_dir__time(LARGE_INTEGER time) {
  return (time.QuadPart - BARE_DIR_EPOCH_OFFSET) * 100;
}

static int
bare_dir__stat(HANDLE handle, bare_dir_stat_t *result) {
  FILE_BASIC_INFO basic;
  FILE_STANDARD_INFO standard;
  FILE_ATTRIBUTE_TAG_INFO tag;
  BY_HANDLE_FILE_INFORMATION info;

  if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) return bare_dir__error();
  if (!GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) return bare_dir__error();
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) return bare_dir__error();
  if (!GetFileInformationByHandle(handle, &info)) return bare_dir__error();

  result->type = bare_dir__type(tag.FileAttributes, tag.ReparseTag);
  result->size = standard.EndOfFile.QuadPart;
  result->ino = ((uint64_t) info.nFileIndexHigh << 32) | info.nFileIndexLow;
  result->nlink = info.nNumberOfLinks;
  result->atime = bare_dir__time(basic.LastAccessTime);
  result->mtime = bare_dir__time(basic.LastWriteTime);
  result->ctime = bare_dir__time(basic.ChangeTime);

  return 0;
}

// Links are refused as on other platforms. Other reparse points are too, as
// opening them without reparse processing doesn't give what they stand for.
static int
bare_dir__refuse_reparse_point(HANDLE handle) {
  FILE_ATTRIBUTE_TAG_INFO tag;

  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) return bare_dir__error();

  if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return IsReparseTagNameSurrogate(tag.ReparseTag) ? UV_ELOOP : UV_ENOTSUP;
  }

  return 0;
}

// Asking for a directory or a file checks the type first, so a link to the
// other type is reported as ELOOP here.
static int
bare_dir__link_error(HANDLE dir, const char *name, int err) {
  if (err != UV_EISDIR && err != UV_ENOTDIR) return err;

  HANDLE handle;

  if (bare_dir__open(dir, name, FILE_READ_ATTRIBUTES, FILE_OPEN, 0, &handle) < 0) return err;

  if (bare_dir__refuse_reparse_point(handle) == UV_ELOOP) err = UV_ELOOP;

  CloseHandle(handle);

  return err;
}

static const ACCESS_MASK bare_dir__directory_access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;

int
bare_dir_open(const char *path, bare_dir_fd_t *result) {
  int err;

  int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (len <= 0) return UV_EINVAL;

  WCHAR *wide = malloc(len * sizeof(WCHAR));
  if (wide == NULL) return UV_ENOMEM;

  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, len);

  // The root is the only directory opened by path.
  HANDLE handle = CreateFileW(
    wide,
    bare_dir__directory_access | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS,
    NULL
  );

  if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
    handle = CreateFileW(
      wide,
      bare_dir__directory_access | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS,
      NULL
    );
  }

  err = handle == INVALID_HANDLE_VALUE ? bare_dir__error() : 0;

  free(wide);

  if (err < 0) return err;

  FILE_ATTRIBUTE_TAG_INFO tag;

  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
    err = bare_dir__error();
  } else if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    err = UV_ENOTDIR;
  }

  if (err < 0) {
    CloseHandle(handle);

    return err;
  }

  *result = handle;

  return 0;
}

int
bare_dir_open_directory(bare_dir_fd_t dir, const char *name, bare_dir_fd_t *result) {
  int err;

  HANDLE handle;
  err = bare_dir__open(dir, name, bare_dir__directory_access, FILE_OPEN, FILE_DIRECTORY_FILE, &handle);
  if (err < 0) return bare_dir__link_error(dir, name, err);

  err = bare_dir__refuse_reparse_point(handle);

  if (err < 0) {
    CloseHandle(handle);

    return err;
  }

  *result = handle;

  return 0;
}

int
bare_dir_open_file(bare_dir_fd_t dir, const char *name, int flags, bare_dir_fd_t *result) {
  int err;

  ACCESS_MASK access = FILE_READ_ATTRIBUTES;

  if (flags & bare_dir_open_read) access |= FILE_READ_DATA;
  if (flags & bare_dir_open_write) access |= FILE_WRITE_DATA | FILE_APPEND_DATA;

  // Without FILE_WRITE_DATA, the kernel only allows writes at the end.
  if (flags & bare_dir_open_append) access |= FILE_APPEND_DATA;

  ULONG disposition;

  if (flags & bare_dir_open_create) {
    if (flags & bare_dir_open_exclusive) disposition = FILE_CREATE;
    else if (flags & bare_dir_open_truncate) disposition = FILE_OVERWRITE_IF;
    else disposition = FILE_OPEN_IF;
  } else {
    disposition = (flags & bare_dir_open_truncate) ? FILE_OVERWRITE : FILE_OPEN;
  }

  HANDLE handle;
  err = bare_dir__open(dir, name, access, disposition, FILE_NON_DIRECTORY_FILE, &handle);
  if (err < 0) return bare_dir__link_error(dir, name, err);

  err = bare_dir__refuse_reparse_point(handle);

  if (err < 0) {
    CloseHandle(handle);

    return err;
  }

  *result = handle;

  return 0;
}

int
bare_dir_create_directory(bare_dir_fd_t dir, const char *name) {
  int err;

  HANDLE handle;
  err = bare_dir__open(dir, name, FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE, &handle);
  if (err < 0) return err;

  CloseHandle(handle);

  return 0;
}

int
bare_dir_stat(bare_dir_fd_t dir, const char *name, bare_dir_stat_t *result) {
  int err;

  HANDLE handle;
  err = bare_dir__open(dir, name, FILE_READ_ATTRIBUTES, FILE_OPEN, 0, &handle);
  if (err < 0) return err;

  err = bare_dir__stat(handle, result);

  CloseHandle(handle);

  return err;
}

int
bare_dir_fstat(bare_dir_fd_t fd, bare_dir_stat_t *result) {
  return bare_dir__stat(fd, result);
}

int
bare_dir_list(bare_dir_fd_t dir, bare_dir_list_cb cb, void *data) {
  int err;

  // An empty name reopens the directory, giving the listing its own position.
  HANDLE handle;
  NTSTATUS status = bare_dir__open_relative(dir, L"", 0, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE, &handle);
  if (!NT_SUCCESS(status)) return bare_dir__status(status);

  _Alignas(LONGLONG) char buffer[64 * 1024];

  FILE_INFO_BY_HANDLE_CLASS info_class = FileIdBothDirectoryRestartInfo;

  err = 0;

  for (;;) {
    if (!GetFileInformationByHandleEx(handle, info_class, buffer, sizeof(buffer))) {
      if (GetLastError() != ERROR_NO_MORE_FILES) err = bare_dir__error();

      break;
    }

    info_class = FileIdBothDirectoryInfo;

    FILE_ID_BOTH_DIR_INFO *entry = (FILE_ID_BOTH_DIR_INFO *) buffer;

    for (;;) {
      int len = entry->FileNameLength / sizeof(WCHAR);

      bool dots = (len == 1 && entry->FileName[0] == L'.') || (len == 2 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.');

      if (!dots) {
        char name[1024];

        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry->FileName, len, name, sizeof(name) - 1, NULL, NULL);

        if (n <= 0) {
          err = GetLastError() == ERROR_NO_UNICODE_TRANSLATION ? UV_EILSEQ : bare_dir__error();

          goto done;
        }

        name[n] = '\0';

        // For reparse points, EaSize holds the reparse tag.
        err = cb(name, bare_dir__type(entry->FileAttributes, entry->EaSize), data);
        if (err != 0) goto done;
      }

      if (entry->NextEntryOffset == 0) break;

      entry = (FILE_ID_BOTH_DIR_INFO *) ((char *) entry + entry->NextEntryOffset);
    }
  }

done:
  CloseHandle(handle);

  return err;
}

int
bare_dir_remove(bare_dir_fd_t dir, const char *name, bool directory) {
  int err;

  HANDLE handle;
  err = bare_dir__open(dir, name, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE, &handle);
  if (err < 0) return err;

  IO_STATUS_BLOCK io;

  // POSIX semantics remove the name immediately, but not every file system
  // supports them.
  bare_dir_disposition_ex_t ex = {
    .Flags = BARE_DIR_FILE_DISPOSITION_DELETE | BARE_DIR_FILE_DISPOSITION_POSIX_SEMANTICS | BARE_DIR_FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE,
  };

  NTSTATUS status = NtSetInformationFile(handle, &io, &ex, sizeof(ex), BARE_DIR_FILE_DISPOSITION_INFORMATION_EX);

  if (bare_dir__unsupported(status)) {
    bare_dir_disposition_t disposition = {.DeleteFile = TRUE};

    status = NtSetInformationFile(handle, &io, &disposition, sizeof(disposition), BARE_DIR_FILE_DISPOSITION_INFORMATION);
  }

  CloseHandle(handle);

  if (!NT_SUCCESS(status)) return bare_dir__status(status);

  return 0;
}

int
bare_dir_rename(bare_dir_fd_t from, const char *name, bare_dir_fd_t to, const char *target, bool replace) {
  int err;

  WCHAR wide[256];
  USHORT len;

  err = bare_dir__to_wide(target, wide, &len);
  if (err < 0) return err;

  HANDLE handle;
  err = bare_dir__open(from, name, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, &handle);
  if (err < 0) return err;

  // Without the root directory handle, the name would be taken as a full path.
  char storage[sizeof(bare_dir_rename_t) + 256 * sizeof(WCHAR)];

  bare_dir_rename_t *info = (bare_dir_rename_t *) storage;

  ULONG size = (ULONG) (offsetof(bare_dir_rename_t, FileName) + len * sizeof(WCHAR));

  memset(storage, 0, sizeof(storage));

  info->Flags = BARE_DIR_FILE_RENAME_POSIX_SEMANTICS | BARE_DIR_FILE_RENAME_IGNORE_READONLY_ATTRIBUTE;

  if (replace) info->Flags |= BARE_DIR_FILE_RENAME_REPLACE_IF_EXISTS;

  info->RootDirectory = to;
  info->FileNameLength = len * sizeof(WCHAR);

  memcpy(info->FileName, wide, len * sizeof(WCHAR));

  IO_STATUS_BLOCK io;

  NTSTATUS status = NtSetInformationFile(handle, &io, info, size, BARE_DIR_FILE_RENAME_INFORMATION_EX);

  if (bare_dir__unsupported(status)) {
    info->Flags = 0;
    info->ReplaceIfExists = replace;

    status = NtSetInformationFile(handle, &io, info, size, BARE_DIR_FILE_RENAME_INFORMATION);
  }

  CloseHandle(handle);

  if (!NT_SUCCESS(status)) return bare_dir__status(status);

  return 0;
}

int
bare_dir_read_link(bare_dir_fd_t dir, const char *name, char *buffer, size_t *len) {
  int err;

  HANDLE handle;
  err = bare_dir__open(dir, name, FILE_READ_ATTRIBUTES, FILE_OPEN, 0, &handle);
  if (err < 0) return err;

  _Alignas(LONGLONG) char data[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  DWORD n;

  BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0, data, sizeof(data), &n, NULL);

  err = ok ? 0 : GetLastError() == ERROR_NOT_A_REPARSE_POINT ? UV_EINVAL
                                                             : bare_dir__error();

  CloseHandle(handle);

  if (err < 0) return err;

  bare_dir_reparse_data_t *reparse = (bare_dir_reparse_data_t *) data;

  const char *path_buffer;
  USHORT offset, length;

  switch (reparse->ReparseTag) {
  case IO_REPARSE_TAG_SYMLINK:
    path_buffer = (const char *) reparse->SymbolicLinkReparseBuffer.PathBuffer;
    offset = reparse->SymbolicLinkReparseBuffer.PrintNameOffset;
    length = reparse->SymbolicLinkReparseBuffer.PrintNameLength;

    if (length == 0) {
      offset = reparse->SymbolicLinkReparseBuffer.SubstituteNameOffset;
      length = reparse->SymbolicLinkReparseBuffer.SubstituteNameLength;
    }
    break;

  case IO_REPARSE_TAG_MOUNT_POINT:
    path_buffer = (const char *) reparse->MountPointReparseBuffer.PathBuffer;
    offset = reparse->MountPointReparseBuffer.PrintNameOffset;
    length = reparse->MountPointReparseBuffer.PrintNameLength;

    if (length == 0) {
      offset = reparse->MountPointReparseBuffer.SubstituteNameOffset;
      length = reparse->MountPointReparseBuffer.SubstituteNameLength;
    }
    break;

  default:
    return UV_EINVAL;
  }

  // Keep the offsets within the returned data.
  if (path_buffer + offset + length > data + n || length % sizeof(WCHAR) != 0) return UV_EINVAL;

  const WCHAR *target = (const WCHAR *) (path_buffer + offset);

  int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, target, length / sizeof(WCHAR), buffer, (int) *len, NULL, NULL);

  if (written <= 0) {
    DWORD error = GetLastError();

    if (error == ERROR_NO_UNICODE_TRANSLATION) return UV_EILSEQ;
    if (error == ERROR_INSUFFICIENT_BUFFER) return UV_ENAMETOOLONG;

    return uv_translate_sys_error(error);
  }

  *len = written;

  return 0;
}

int
bare_dir_read(bare_dir_fd_t fd, void *buffer, size_t len, uint64_t position, size_t *result) {
  OVERLAPPED overlapped = {
    .Offset = (DWORD) position,
    .OffsetHigh = (DWORD) (position >> 32),
  };

  DWORD n;

  if (!ReadFile(fd, buffer, len > MAXDWORD ? MAXDWORD : (DWORD) len, &n, &overlapped)) {
    if (GetLastError() != ERROR_HANDLE_EOF) return bare_dir__error();

    n = 0;
  }

  *result = n;

  return 0;
}

int
bare_dir_write(bare_dir_fd_t fd, const void *buffer, size_t len, uint64_t position, size_t *result) {
  OVERLAPPED overlapped = {
    .Offset = (DWORD) position,
    .OffsetHigh = (DWORD) (position >> 32),
  };

  DWORD n;

  if (!WriteFile(fd, buffer, len > MAXDWORD ? MAXDWORD : (DWORD) len, &n, &overlapped)) return bare_dir__error();

  *result = n;

  return 0;
}

int
bare_dir_append(bare_dir_fd_t fd, const void *buffer, size_t len, size_t *result) {
  // An offset of all ones appends.
  OVERLAPPED overlapped = {
    .Offset = 0xFFFFFFFF,
    .OffsetHigh = 0xFFFFFFFF,
  };

  DWORD n;

  if (!WriteFile(fd, buffer, len > MAXDWORD ? MAXDWORD : (DWORD) len, &n, &overlapped)) return bare_dir__error();

  *result = n;

  return 0;
}

int
bare_dir_truncate(bare_dir_fd_t fd, uint64_t size) {
  FILE_END_OF_FILE_INFO info = {.EndOfFile.QuadPart = (LONGLONG) size};

  if (!SetFileInformationByHandle(fd, FileEndOfFileInfo, &info, sizeof(info))) return bare_dir__error();

  return 0;
}

int
bare_dir_sync(bare_dir_fd_t fd) {
  if (!FlushFileBuffers(fd)) return bare_dir__error();

  return 0;
}

static int
bare_dir__file_time(const int64_t *time, LARGE_INTEGER *result) {
  // A zero time means unchanged, so times before 1601 can't be set.
  int64_t ticks = *time / 100;

  if (*time % 100 < 0) ticks -= 1;

  ticks += BARE_DIR_EPOCH_OFFSET;

  if (ticks <= 0) return UV_EINVAL;

  result->QuadPart = ticks;

  return 0;
}

int
bare_dir_set_times(bare_dir_fd_t fd, const int64_t *atime, const int64_t *mtime) {
  int err;

  FILE_BASIC_INFO info;
  memset(&info, 0, sizeof(info));

  if (atime) {
    err = bare_dir__file_time(atime, &info.LastAccessTime);
    if (err < 0) return err;
  }

  if (mtime) {
    err = bare_dir__file_time(mtime, &info.LastWriteTime);
    if (err < 0) return err;
  }

  if (!SetFileInformationByHandle(fd, FileBasicInfo, &info, sizeof(info))) return bare_dir__error();

  return 0;
}

int
bare_dir_duplicate(bare_dir_fd_t fd, bare_dir_fd_t *result) {
  HANDLE process = GetCurrentProcess();

  if (!DuplicateHandle(process, fd, process, result, 0, FALSE, DUPLICATE_SAME_ACCESS)) return bare_dir__error();

  return 0;
}

void
bare_dir_close(bare_dir_fd_t fd) {
  CloseHandle(fd);
}
