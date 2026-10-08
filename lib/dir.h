#ifndef BARE_DIR_H
#define BARE_DIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

// Functions return 0 or a negative libuv error code. Names have already passed
// bare_dir_valid_name().

typedef uv_os_fd_t bare_dir_fd_t;

enum {
  bare_dir_type_file = 1,
  bare_dir_type_directory = 2,
  bare_dir_type_symlink = 3,
  bare_dir_type_other = 4,
};

enum {
  bare_dir_open_read = 1,
  bare_dir_open_write = 2,
  bare_dir_open_append = 4,
  bare_dir_open_create = 8,
  bare_dir_open_exclusive = 16,
  bare_dir_open_truncate = 32,
};

typedef struct {
  int type;
  uint64_t size;
  uint64_t ino;
  uint64_t nlink;
  int64_t atime;
  int64_t mtime;
  int64_t ctime;
} bare_dir_stat_t;

typedef int (*bare_dir_list_cb)(const char *name, int type, void *data);

bool
bare_dir_valid_name(const char *name, size_t len);

int
bare_dir_open(const char *path, bare_dir_fd_t *result);

int
bare_dir_open_directory(bare_dir_fd_t dir, const char *name, bare_dir_fd_t *result);

int
bare_dir_open_file(bare_dir_fd_t dir, const char *name, int flags, bare_dir_fd_t *result);

int
bare_dir_create_directory(bare_dir_fd_t dir, const char *name);

int
bare_dir_stat(bare_dir_fd_t dir, const char *name, bare_dir_stat_t *result);

int
bare_dir_fstat(bare_dir_fd_t fd, bare_dir_stat_t *result);

int
bare_dir_list(bare_dir_fd_t dir, bare_dir_list_cb cb, void *data);

int
bare_dir_remove(bare_dir_fd_t dir, const char *name, bool directory);

int
bare_dir_rename(bare_dir_fd_t from, const char *name, bare_dir_fd_t to, const char *target, bool replace);

int
bare_dir_read_link(bare_dir_fd_t dir, const char *name, char *buffer, size_t *len);

int
bare_dir_read(bare_dir_fd_t fd, void *buffer, size_t len, uint64_t position, size_t *result);

int
bare_dir_write(bare_dir_fd_t fd, const void *buffer, size_t len, uint64_t position, size_t *result);

int
bare_dir_append(bare_dir_fd_t fd, const void *buffer, size_t len, size_t *result);

int
bare_dir_truncate(bare_dir_fd_t fd, uint64_t size);

int
bare_dir_sync(bare_dir_fd_t fd);

int
bare_dir_set_times(bare_dir_fd_t fd, const int64_t *atime, const int64_t *mtime);

int
bare_dir_duplicate(bare_dir_fd_t fd, bare_dir_fd_t *result);

void
bare_dir_close(bare_dir_fd_t fd);

#endif // BARE_DIR_H
