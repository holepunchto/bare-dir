#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>

#include "lib/dir.h"

#define BARE_DIR_NAME_MAX 255
#define BARE_DIR_LINK_MAX 4096

enum {
  bare_dir_right_list = 1 << 0,
  bare_dir_right_lookup = 1 << 1,
  bare_dir_right_create = 1 << 2,
  bare_dir_right_remove = 1 << 3,
  bare_dir_right_rename = 1 << 4,
  bare_dir_right_read = 1 << 5,
  bare_dir_right_write = 1 << 6,
  bare_dir_right_append = 1 << 7,
  bare_dir_right_truncate = 1 << 8,
  bare_dir_right_set_times = 1 << 9,

  bare_dir_rights_all = (1 << 10) - 1,
};

typedef struct {
  bare_dir_fd_t fd;
  uint32_t rights;
  bool closed;

  // Operations on the threadpool that still use the descriptor, which is only
  // closed once they're done.
  uint32_t pending;
} bare_dir_handle_t;

typedef enum {
  bare_dir_op_open,
  bare_dir_op_open_directory,
  bare_dir_op_open_file,
  bare_dir_op_create_directory,
  bare_dir_op_stat,
  bare_dir_op_fstat,
  bare_dir_op_list,
  bare_dir_op_remove,
  bare_dir_op_rename,
  bare_dir_op_read_link,
  bare_dir_op_read,
  bare_dir_op_write,
  bare_dir_op_append,
  bare_dir_op_truncate,
  bare_dir_op_sync,
  bare_dir_op_set_times,
} bare_dir_op_type_t;

typedef struct {
  char *name;
  int type;
} bare_dir_entry_t;

typedef struct {
  uv_work_t work;
  js_env_t *env;
  js_deferred_t *deferred;

  bool exiting;

  js_deferred_teardown_t *teardown;

  bare_dir_op_type_t type;

  bare_dir_handle_t *handle;
  bare_dir_handle_t *target;
  js_value_t *values[3];
  js_ref_t *refs[3];

  char *path;
  char name[BARE_DIR_NAME_MAX + 1];
  char target_name[BARE_DIR_NAME_MAX + 1];
  uint32_t flags;
  uint32_t rights;
  bool directory;
  bool replace;
  js_arraybuffer_backing_store_t *backing_store;
  void *data;
  size_t len;
  uint64_t position;
  int64_t atime;
  int64_t mtime;
  bool has_atime;
  bool has_mtime;

  int err;
  bare_dir_fd_t fd;
  bare_dir_stat_t stat;
  size_t n;
  bare_dir_entry_t *entries;
  size_t entries_len;
  size_t entries_cap;
  char link[BARE_DIR_LINK_MAX];
  size_t link_len;
} bare_dir_op_t;

static js_type_tag_t bare_dir__directory_tag = {0x5f6a2c4e8d1b3a70, 0x9c47e2d0b5a81f36};

static js_type_tag_t bare_dir__file_tag = {0x2b8e61f4c03d9a57, 0xe4a7019c6d52b83f};

// An invalid name here means the JavaScript checks were bypassed. Carrying on
// could allow an escape, so the process is aborted.
static void
bare_dir__invalid_name(void) {
  fprintf(stderr, "bare-dir: an invalid name reached the native layer\n");

  abort();
}

static js_value_t *
bare_dir__create_error(js_env_t *env, const char *code, const char *message) {
  int err;

  js_value_t *values[2];

  err = js_create_string_utf8(env, (const utf8_t *) code, -1, &values[0]);
  assert(err == 0);

  err = js_create_string_utf8(env, (const utf8_t *) message, -1, &values[1]);
  assert(err == 0);

  js_value_t *result;
  err = js_create_error(env, values[0], values[1], &result);
  assert(err == 0);

  return result;
}

static void
bare_dir__throw(js_env_t *env, int code) {
  int err = js_throw(env, bare_dir__create_error(env, uv_err_name(code), uv_strerror(code)));
  assert(err == 0);
}

static void
bare_dir__throw_not_capable(js_env_t *env) {
  int err = js_throw(env, bare_dir__create_error(env, "ENOTCAPABLE", "capabilities insufficient"));
  assert(err == 0);
}

static bool
bare_dir__read_name(js_env_t *env, js_value_t *value, char name[BARE_DIR_NAME_MAX + 1], size_t *len) {
  int err;

  err = js_get_value_string_utf8(env, value, NULL, 0, len);
  assert(err == 0);

  if (*len > BARE_DIR_NAME_MAX) return false;

  err = js_get_value_string_utf8(env, value, (utf8_t *) name, *len, len);
  assert(err == 0);

  name[*len] = '\0';

  return bare_dir_valid_name(name, *len);
}

static void
bare_dir__get_name(js_env_t *env, js_value_t *value, char name[BARE_DIR_NAME_MAX + 1]) {
  size_t len;

  if (!bare_dir__read_name(env, value, name, &len)) bare_dir__invalid_name();
}

static bool
bare_dir__get_bool(js_env_t *env, js_value_t *value) {
  int err;

  bool result;
  err = js_get_value_bool(env, value, &result);
  assert(err == 0);

  return result;
}

static uint32_t
bare_dir__get_uint32(js_env_t *env, js_value_t *value) {
  int err;

  uint32_t result;
  err = js_get_value_uint32(env, value, &result);
  assert(err == 0);

  return result;
}

static void
bare_dir__finalize(js_env_t *env, void *data, void *finalize_hint) {
  bare_dir_handle_t *handle = data;

  if (!handle->closed) bare_dir_close(handle->fd);

  free(handle);
}

static js_value_t *
bare_dir__wrap(js_env_t *env, bare_dir_fd_t fd, uint32_t rights, const js_type_tag_t *tag) {
  int err;

  bare_dir_handle_t *handle = malloc(sizeof(bare_dir_handle_t));
  assert(handle);

  handle->fd = fd;
  handle->rights = rights;
  handle->closed = false;
  handle->pending = 0;

  js_value_t *result;
  err = js_create_object(env, &result);
  assert(err == 0);

  err = js_wrap(env, result, handle, bare_dir__finalize, NULL, NULL);
  assert(err == 0);

  err = js_add_type_tag(env, result, tag);
  assert(err == 0);

  return result;
}

static bool
bare_dir__has_tag(js_env_t *env, js_value_t *value, const js_type_tag_t *tag) {
  int err;

  js_value_type_t type;
  err = js_typeof(env, value, &type);
  assert(err == 0);

  if (type != js_object) return false;

  bool result;
  err = js_check_type_tag(env, value, tag, &result);
  assert(err == 0);

  return result;
}

static bare_dir_handle_t *
bare_dir__unwrap(js_env_t *env, js_value_t *value, const js_type_tag_t *tag) {
  int err;

  if (!bare_dir__has_tag(env, value, tag)) {
    err = js_throw_type_error(env, NULL, tag == &bare_dir__directory_tag ? "Not a directory handle" : "Not a file handle");
    assert(err == 0);

    return NULL;
  }

  bare_dir_handle_t *handle;
  err = js_unwrap(env, value, (void **) &handle);
  assert(err == 0);

  if (handle->closed) {
    bare_dir__throw(env, UV_EBADF);

    return NULL;
  }

  return handle;
}

static bare_dir_handle_t *
bare_dir__unwrap_any(js_env_t *env, js_value_t *value, const js_type_tag_t **tag) {
  *tag = bare_dir__has_tag(env, value, &bare_dir__directory_tag) ? &bare_dir__directory_tag : &bare_dir__file_tag;

  return bare_dir__unwrap(env, value, *tag);
}

static bool
bare_dir__require(js_env_t *env, bare_dir_handle_t *handle, uint32_t rights) {
  if ((handle->rights & rights) == rights) return true;

  bare_dir__throw_not_capable(env);

  return false;
}

static bool
bare_dir__get_buffer(js_env_t *env, js_value_t *value, bare_dir_op_t *op) {
  int err;

  bool is_typedarray;
  err = js_is_typedarray(env, value, &is_typedarray);
  assert(err == 0);

  js_typedarray_type_t type = js_int8array;
  js_value_t *buffer;
  size_t offset;

  if (is_typedarray) {
    err = js_get_typedarray_info(env, value, &type, NULL, &op->len, &buffer, &offset);
    assert(err == 0);
  }

  if (type != js_uint8array) {
    err = js_throw_type_error(env, NULL, "Buffer must be a Uint8Array");
    assert(err == 0);

    return false;
  }

  bool shared;
  err = js_is_sharedarraybuffer(env, buffer, &shared);
  assert(err == 0);

  // Asking for the buffer may move the data out of the heap, and the backing
  // store keeps it alive should the buffer be detached on the threadpool.
  void *data;

  if (shared) {
    err = js_get_sharedarraybuffer_info(env, buffer, &data, NULL);
    assert(err == 0);

    err = js_get_sharedarraybuffer_backing_store(env, buffer, &op->backing_store);
    assert(err == 0);
  } else {
    err = js_get_arraybuffer_info(env, buffer, &data, NULL);
    assert(err == 0);

    err = js_get_arraybuffer_backing_store(env, buffer, &op->backing_store);
    assert(err == 0);
  }

  op->data = (char *) data + offset;

  return true;
}

static bool
bare_dir__get_position(js_env_t *env, js_value_t *value, uint64_t *result) {
  int err;

  int64_t position;
  err = js_get_value_int64(env, value, &position);
  assert(err == 0);

  if (position < 0) {
    bare_dir__throw(env, UV_EINVAL);

    return false;
  }

  *result = position;

  return true;
}

static bool
bare_dir__get_time(js_env_t *env, js_value_t *value, int64_t *time, bool *has_time) {
  int err;

  js_value_type_t type;
  err = js_typeof(env, value, &type);
  assert(err == 0);

  if (type == js_null || type == js_undefined) {
    *has_time = false;

    return true;
  }

  if (type != js_bigint) {
    err = js_throw_type_error(env, NULL, "Time must be a bigint");
    assert(err == 0);

    return false;
  }

  bool lossless;
  err = js_get_value_bigint_int64(env, value, time, &lossless);
  assert(err == 0);

  if (!lossless) {
    bare_dir__throw(env, UV_EINVAL);

    return false;
  }

  *has_time = true;

  return true;
}

static bare_dir_op_t *
bare_dir__op(js_env_t *env, bare_dir_op_type_t type) {
  bare_dir_op_t *op = calloc(1, sizeof(bare_dir_op_t));
  assert(op);

  op->env = env;
  op->type = type;

  return op;
}

static void
bare_dir__op_free(bare_dir_op_t *op) {
  int err;

  if (op->backing_store) {
    err = js_release_arraybuffer_backing_store(op->env, op->backing_store);
    assert(err == 0);
  }

  for (size_t i = 0; i < op->entries_len; i++) {
    free(op->entries[i].name);
  }

  free(op->entries);
  free(op->path);
  free(op);
}

static int
bare_dir__on_entry(const char *name, int type, void *data) {
  bare_dir_op_t *op = data;

  size_t len = strlen(name);

  // A name that isn't valid UTF-8 can't round-trip through a string.
  if (!utf8_validate((const utf8_t *) name, len)) return UV_EILSEQ;

  if (op->entries_len == op->entries_cap) {
    size_t cap = op->entries_cap ? op->entries_cap * 2 : 16;

    bare_dir_entry_t *entries = realloc(op->entries, cap * sizeof(bare_dir_entry_t));

    if (entries == NULL) return UV_ENOMEM;

    op->entries = entries;
    op->entries_cap = cap;
  }

  char *copy = malloc(len + 1);

  if (copy == NULL) return UV_ENOMEM;

  memcpy(copy, name, len + 1);

  op->entries[op->entries_len++] = (bare_dir_entry_t){copy, type};

  return 0;
}

// Runs the operation without touching JavaScript, which may be on another
// thread.
static void
bare_dir__run(bare_dir_op_t *op) {
  bare_dir_fd_t fd = op->handle ? op->handle->fd : 0;

  switch (op->type) {
  case bare_dir_op_open:
    op->err = bare_dir_open(op->path, &op->fd);
    break;

  case bare_dir_op_open_directory:
    op->err = bare_dir_open_directory(fd, op->name, &op->fd);
    break;

  case bare_dir_op_open_file:
    op->err = bare_dir_open_file(fd, op->name, op->flags, &op->fd);
    break;

  case bare_dir_op_create_directory:
    op->err = bare_dir_create_directory(fd, op->name);
    break;

  case bare_dir_op_stat:
    op->err = bare_dir_stat(fd, op->name, &op->stat);
    break;

  case bare_dir_op_fstat:
    op->err = bare_dir_fstat(fd, &op->stat);
    break;

  case bare_dir_op_list:
    op->err = bare_dir_list(fd, bare_dir__on_entry, op);
    break;

  case bare_dir_op_remove:
    op->err = bare_dir_remove(fd, op->name, op->directory);
    break;

  case bare_dir_op_rename:
    op->err = bare_dir_rename(fd, op->name, op->target->fd, op->target_name, op->replace);
    break;

  case bare_dir_op_read_link:
    op->link_len = sizeof(op->link);
    op->err = bare_dir_read_link(fd, op->name, op->link, &op->link_len);

    if (op->err == 0 && !utf8_validate((const utf8_t *) op->link, op->link_len))
      op->err = UV_EILSEQ;
    break;

  case bare_dir_op_read:
    op->err = bare_dir_read(fd, op->data, op->len, op->position, &op->n);
    break;

  case bare_dir_op_write:
    op->err = bare_dir_write(fd, op->data, op->len, op->position, &op->n);
    break;

  case bare_dir_op_append:
    op->err = bare_dir_append(fd, op->data, op->len, &op->n);
    break;

  case bare_dir_op_truncate:
    op->err = bare_dir_truncate(fd, op->position);
    break;

  case bare_dir_op_sync:
    op->err = bare_dir_sync(fd);
    break;

  case bare_dir_op_set_times:
    op->err = bare_dir_set_times(fd, op->has_atime ? &op->atime : NULL, op->has_mtime ? &op->mtime : NULL);
    break;
  }
}

static js_value_t *
bare_dir__create_stat(js_env_t *env, bare_dir_stat_t *stat) {
  int err;

  js_value_t *result;
  err = js_create_array_with_length(env, 7, &result);
  assert(err == 0);

  js_value_t *values[7];

  err = js_create_uint32(env, stat->type, &values[0]);
  assert(err == 0);

  err = js_create_bigint_uint64(env, stat->size, &values[1]);
  assert(err == 0);

  err = js_create_bigint_uint64(env, stat->ino, &values[2]);
  assert(err == 0);

  err = js_create_bigint_uint64(env, stat->nlink, &values[3]);
  assert(err == 0);

  err = js_create_bigint_int64(env, stat->atime, &values[4]);
  assert(err == 0);

  err = js_create_bigint_int64(env, stat->mtime, &values[5]);
  assert(err == 0);

  err = js_create_bigint_int64(env, stat->ctime, &values[6]);
  assert(err == 0);

  for (uint32_t i = 0; i < 7; i++) {
    err = js_set_element(env, result, i, values[i]);
    assert(err == 0);
  }

  return result;
}

static js_value_t *
bare_dir__create_entries(js_env_t *env, bare_dir_op_t *op) {
  int err;

  js_value_t *result;
  err = js_create_array_with_length(env, op->entries_len, &result);
  assert(err == 0);

  for (size_t i = 0; i < op->entries_len; i++) {
    js_value_t *entry;
    err = js_create_array_with_length(env, 2, &entry);
    assert(err == 0);

    js_value_t *value;
    err = js_create_string_utf8(env, (const utf8_t *) op->entries[i].name, -1, &value);
    assert(err == 0);

    err = js_set_element(env, entry, 0, value);
    assert(err == 0);

    err = js_create_uint32(env, op->entries[i].type, &value);
    assert(err == 0);

    err = js_set_element(env, entry, 1, value);
    assert(err == 0);

    err = js_set_element(env, result, (uint32_t) i, entry);
    assert(err == 0);
  }

  return result;
}

static js_value_t *
bare_dir__result(js_env_t *env, bare_dir_op_t *op) {
  int err;

  js_value_t *result;

  switch (op->type) {
  case bare_dir_op_open:
  case bare_dir_op_open_directory:
    return bare_dir__wrap(env, op->fd, op->rights, &bare_dir__directory_tag);

  case bare_dir_op_open_file:
    return bare_dir__wrap(env, op->fd, op->rights, &bare_dir__file_tag);

  case bare_dir_op_stat:
  case bare_dir_op_fstat:
    return bare_dir__create_stat(env, &op->stat);

  case bare_dir_op_list:
    return bare_dir__create_entries(env, op);

  case bare_dir_op_read_link:
    err = js_create_string_utf8(env, (const utf8_t *) op->link, op->link_len, &result);
    assert(err == 0);

    return result;

  case bare_dir_op_read:
  case bare_dir_op_write:
  case bare_dir_op_append:
    err = js_create_int64(env, (int64_t) op->n, &result);
    assert(err == 0);

    return result;

  default:
    err = js_get_undefined(env, &result);
    assert(err == 0);

    return result;
  }
}

static void
bare_dir__release(bare_dir_handle_t *handle) {
  if (handle == NULL) return;

  if (--handle->pending == 0 && handle->closed) bare_dir_close(handle->fd);
}

static void
bare_dir__on_work(uv_work_t *req) {
  bare_dir__run((bare_dir_op_t *) req->data);
}

static void
bare_dir__settle(js_env_t *env, bare_dir_op_t *op) {
  int err;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  if (op->err < 0) {
    err = js_reject_deferred(env, op->deferred, bare_dir__create_error(env, uv_err_name(op->err), uv_strerror(op->err)));
  } else {
    err = js_resolve_deferred(env, op->deferred, bare_dir__result(env, op));
  }

  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);
}

static void
bare_dir__discard(bare_dir_op_t *op) {
  if (op->err < 0) return;

  switch (op->type) {
  case bare_dir_op_open:
  case bare_dir_op_open_directory:
  case bare_dir_op_open_file:
    bare_dir_close(op->fd);
    break;

  default:
    break;
  }
}

static void
bare_dir__on_after_work(uv_work_t *req, int status) {
  int err;

  bare_dir_op_t *op = req->data;

  js_env_t *env = op->env;

  if (status < 0) op->err = status;

  bare_dir__release(op->handle);
  bare_dir__release(op->target);

  for (int i = 0; i < 3; i++) {
    if (op->refs[i] == NULL) continue;

    err = js_delete_reference(env, op->refs[i]);
    assert(err == 0);
  }

  if (op->exiting) bare_dir__discard(op);
  else bare_dir__settle(env, op);

  js_deferred_teardown_t *teardown = op->teardown;

  bare_dir__op_free(op);

  err = js_finish_deferred_teardown_callback(teardown);
  assert(err == 0);
}

static void
bare_dir__on_teardown(js_deferred_teardown_t *handle, void *data) {
  bare_dir_op_t *op = data;

  op->exiting = true;

  uv_cancel((uv_req_t *) &op->work);
}

static js_value_t *
bare_dir__dispatch(js_env_t *env, bare_dir_op_t *op, bool async) {
  int err;

  if (!async) {
    bare_dir__run(op);

    js_value_t *result = NULL;

    if (op->err < 0) bare_dir__throw(env, op->err);
    else result = bare_dir__result(env, op);

    bare_dir__op_free(op);

    return result;
  }

  for (int i = 0; i < 3; i++) {
    if (op->values[i] == NULL) continue;

    err = js_create_reference(env, op->values[i], 1, &op->refs[i]);
    assert(err == 0);
  }

  if (op->handle) op->handle->pending++;
  if (op->target) op->target->pending++;

  js_value_t *promise;
  err = js_create_promise(env, &op->deferred, &promise);
  assert(err == 0);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  err = js_add_deferred_teardown_callback(env, bare_dir__on_teardown, (void *) op, &op->teardown);
  assert(err == 0);

  op->work.data = op;

  err = uv_queue_work(loop, &op->work, bare_dir__on_work, bare_dir__on_after_work);
  assert(err == 0);

  return promise;
}

static js_value_t *
bare_dir_valid_name_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  char name[BARE_DIR_NAME_MAX + 1];
  size_t len;

  js_value_t *result;
  err = js_get_boolean(env, bare_dir__read_name(env, argv[0], name, &len), &result);
  assert(err == 0);

  return result;
}

static js_value_t *
bare_dir_open_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  size_t len;
  err = js_get_value_string_utf8(env, argv[0], NULL, 0, &len);
  assert(err == 0);

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_open);

  op->path = malloc(len + 1);
  assert(op->path);

  err = js_get_value_string_utf8(env, argv[0], (utf8_t *) op->path, len, &len);
  assert(err == 0);

  op->path[len] = '\0';

  if (memchr(op->path, '\0', len)) {
    bare_dir__op_free(op);

    bare_dir__throw(env, UV_EINVAL);

    return NULL;
  }

  op->rights = bare_dir__get_uint32(env, argv[1]) & bare_dir_rights_all;

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[2]));
}

static js_value_t *
bare_dir_open_directory_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *dir = bare_dir__unwrap(env, argv[0], &bare_dir__directory_tag);

  if (dir == NULL) return NULL;

  if (!bare_dir__require(env, dir, bare_dir_right_lookup)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_open_directory);

  bare_dir__get_name(env, argv[1], op->name);

  op->handle = dir;
  op->values[0] = argv[0];
  op->rights = dir->rights;

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[2]));
}

static js_value_t *
bare_dir_open_file_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *dir = bare_dir__unwrap(env, argv[0], &bare_dir__directory_tag);

  if (dir == NULL) return NULL;

  uint32_t flags = bare_dir__get_uint32(env, argv[2]);

  uint32_t required = bare_dir_right_lookup;

  if (flags & bare_dir_open_read) required |= bare_dir_right_read;
  if (flags & bare_dir_open_write) required |= bare_dir_right_write;
  if (flags & bare_dir_open_append) required |= bare_dir_right_append;
  if (flags & bare_dir_open_create) required |= bare_dir_right_create;
  if (flags & bare_dir_open_truncate) required |= bare_dir_right_truncate;

  if (!bare_dir__require(env, dir, required)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_open_file);

  bare_dir__get_name(env, argv[1], op->name);

  op->handle = dir;
  op->values[0] = argv[0];
  op->flags = flags;

  // A file has the rights it was opened for, limited by those of its directory.
  op->rights = dir->rights & bare_dir_right_set_times;

  if (flags & bare_dir_open_read) op->rights |= bare_dir_right_read;
  if (flags & bare_dir_open_write) op->rights |= bare_dir_right_write | (dir->rights & bare_dir_right_truncate);
  if (flags & bare_dir_open_append) op->rights |= bare_dir_right_append;

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[3]));
}

static js_value_t *
bare_dir__name_op(js_env_t *env, js_callback_info_t *info, bare_dir_op_type_t type, uint32_t required) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *dir = bare_dir__unwrap(env, argv[0], &bare_dir__directory_tag);

  if (dir == NULL) return NULL;

  if (!bare_dir__require(env, dir, required)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, type);

  bare_dir__get_name(env, argv[1], op->name);

  op->handle = dir;
  op->values[0] = argv[0];

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[2]));
}

static js_value_t *
bare_dir_create_directory_binding(js_env_t *env, js_callback_info_t *info) {
  return bare_dir__name_op(env, info, bare_dir_op_create_directory, bare_dir_right_create);
}

static js_value_t *
bare_dir_stat_binding(js_env_t *env, js_callback_info_t *info) {
  return bare_dir__name_op(env, info, bare_dir_op_stat, bare_dir_right_lookup);
}

static js_value_t *
bare_dir_read_link_binding(js_env_t *env, js_callback_info_t *info) {
  return bare_dir__name_op(env, info, bare_dir_op_read_link, bare_dir_right_lookup);
}

static js_value_t *
bare_dir_fstat_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  const js_type_tag_t *tag;

  bare_dir_handle_t *handle = bare_dir__unwrap_any(env, argv[0], &tag);

  if (handle == NULL) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_fstat);

  op->handle = handle;
  op->values[0] = argv[0];

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[1]));
}

static js_value_t *
bare_dir_list_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *dir = bare_dir__unwrap(env, argv[0], &bare_dir__directory_tag);

  if (dir == NULL) return NULL;

  if (!bare_dir__require(env, dir, bare_dir_right_list)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_list);

  op->handle = dir;
  op->values[0] = argv[0];

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[1]));
}

static js_value_t *
bare_dir_remove_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *dir = bare_dir__unwrap(env, argv[0], &bare_dir__directory_tag);

  if (dir == NULL) return NULL;

  if (!bare_dir__require(env, dir, bare_dir_right_remove)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_remove);

  bare_dir__get_name(env, argv[1], op->name);

  op->handle = dir;
  op->values[0] = argv[0];
  op->directory = bare_dir__get_bool(env, argv[2]);

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[3]));
}

static js_value_t *
bare_dir_rename_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 6;
  js_value_t *argv[6];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *from = bare_dir__unwrap(env, argv[0], &bare_dir__directory_tag);

  if (from == NULL) return NULL;

  bare_dir_handle_t *to = bare_dir__unwrap(env, argv[2], &bare_dir__directory_tag);

  if (to == NULL) return NULL;

  bool replace = bare_dir__get_bool(env, argv[4]);

  if (!bare_dir__require(env, from, bare_dir_right_rename)) return NULL;
  if (!bare_dir__require(env, to, bare_dir_right_rename)) return NULL;

  // Replacing an entry removes it.
  if (replace && !bare_dir__require(env, to, bare_dir_right_remove)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_rename);

  bare_dir__get_name(env, argv[1], op->name);
  bare_dir__get_name(env, argv[3], op->target_name);

  op->handle = from;
  op->target = to;
  op->values[0] = argv[0];
  op->values[1] = argv[2];
  op->replace = replace;

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[5]));
}

static js_value_t *
bare_dir_read_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *file = bare_dir__unwrap(env, argv[0], &bare_dir__file_tag);

  if (file == NULL) return NULL;

  if (!bare_dir__require(env, file, bare_dir_right_read)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_read);

  if (!bare_dir__get_buffer(env, argv[1], op) || !bare_dir__get_position(env, argv[2], &op->position)) {
    bare_dir__op_free(op);

    return NULL;
  }

  op->handle = file;
  op->values[0] = argv[0];

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[3]));
}

static js_value_t *
bare_dir_write_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *file = bare_dir__unwrap(env, argv[0], &bare_dir__file_tag);

  if (file == NULL) return NULL;

  if (!(file->rights & (bare_dir_right_write | bare_dir_right_append))) {
    bare_dir__throw_not_capable(env);

    return NULL;
  }

  // Append-only files were opened for appending, so writes land at the end.
  bool append = !(file->rights & bare_dir_right_write);

  bare_dir_op_t *op = bare_dir__op(env, append ? bare_dir_op_append : bare_dir_op_write);

  if (!bare_dir__get_buffer(env, argv[1], op) || (!append && !bare_dir__get_position(env, argv[2], &op->position))) {
    bare_dir__op_free(op);

    return NULL;
  }

  op->handle = file;
  op->values[0] = argv[0];

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[3]));
}

static js_value_t *
bare_dir_truncate_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *file = bare_dir__unwrap(env, argv[0], &bare_dir__file_tag);

  if (file == NULL) return NULL;

  if (!bare_dir__require(env, file, bare_dir_right_truncate)) return NULL;

  uint64_t size;
  if (!bare_dir__get_position(env, argv[1], &size)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_truncate);

  op->handle = file;
  op->values[0] = argv[0];
  op->position = size;

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[2]));
}

static js_value_t *
bare_dir_sync_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  bare_dir_handle_t *file = bare_dir__unwrap(env, argv[0], &bare_dir__file_tag);

  if (file == NULL) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_sync);

  op->handle = file;
  op->values[0] = argv[0];

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[1]));
}

static js_value_t *
bare_dir_set_times_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  const js_type_tag_t *tag;

  bare_dir_handle_t *handle = bare_dir__unwrap_any(env, argv[0], &tag);

  if (handle == NULL) return NULL;

  if (!bare_dir__require(env, handle, bare_dir_right_set_times)) return NULL;

  int64_t atime = 0, mtime = 0;
  bool has_atime, has_mtime;

  if (!bare_dir__get_time(env, argv[1], &atime, &has_atime)) return NULL;
  if (!bare_dir__get_time(env, argv[2], &mtime, &has_mtime)) return NULL;

  bare_dir_op_t *op = bare_dir__op(env, bare_dir_op_set_times);

  op->handle = handle;
  op->values[0] = argv[0];
  op->atime = atime;
  op->mtime = mtime;
  op->has_atime = has_atime;
  op->has_mtime = has_mtime;

  return bare_dir__dispatch(env, op, bare_dir__get_bool(env, argv[3]));
}

static js_value_t *
bare_dir_restrict_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  const js_type_tag_t *tag;

  bare_dir_handle_t *handle = bare_dir__unwrap_any(env, argv[0], &tag);

  if (handle == NULL) return NULL;

  uint32_t rights = bare_dir__get_uint32(env, argv[1]);

  bare_dir_fd_t fd;
  err = bare_dir_duplicate(handle->fd, &fd);

  if (err < 0) {
    bare_dir__throw(env, err);

    return NULL;
  }

  return bare_dir__wrap(env, fd, handle->rights & rights, tag);
}

static js_value_t *
bare_dir_rights_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  const js_type_tag_t *tag;

  bare_dir_handle_t *handle = bare_dir__unwrap_any(env, argv[0], &tag);

  if (handle == NULL) return NULL;

  js_value_t *result;
  err = js_create_uint32(env, handle->rights, &result);
  assert(err == 0);

  return result;
}

static js_value_t *
bare_dir_close_binding(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  const js_type_tag_t *tag;

  bare_dir_handle_t *handle = bare_dir__unwrap_any(env, argv[0], &tag);

  if (handle == NULL) return NULL;

  handle->closed = true;

  if (handle->pending == 0) bare_dir_close(handle->fd);

  return NULL;
}

static js_value_t *
bare_dir_exports(js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("validName", bare_dir_valid_name_binding)
  V("open", bare_dir_open_binding)
  V("openDirectory", bare_dir_open_directory_binding)
  V("openFile", bare_dir_open_file_binding)
  V("createDirectory", bare_dir_create_directory_binding)
  V("stat", bare_dir_stat_binding)
  V("fstat", bare_dir_fstat_binding)
  V("list", bare_dir_list_binding)
  V("remove", bare_dir_remove_binding)
  V("rename", bare_dir_rename_binding)
  V("readLink", bare_dir_read_link_binding)
  V("read", bare_dir_read_binding)
  V("write", bare_dir_write_binding)
  V("truncate", bare_dir_truncate_binding)
  V("sync", bare_dir_sync_binding)
  V("setTimes", bare_dir_set_times_binding)
  V("restrict", bare_dir_restrict_binding)
  V("rights", bare_dir_rights_binding)
  V("close", bare_dir_close_binding)
#undef V

#define V(name, value) \
  { \
    js_value_t *val; \
    err = js_create_uint32(env, value, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("RIGHT_LIST", bare_dir_right_list)
  V("RIGHT_LOOKUP", bare_dir_right_lookup)
  V("RIGHT_CREATE", bare_dir_right_create)
  V("RIGHT_REMOVE", bare_dir_right_remove)
  V("RIGHT_RENAME", bare_dir_right_rename)
  V("RIGHT_READ", bare_dir_right_read)
  V("RIGHT_WRITE", bare_dir_right_write)
  V("RIGHT_APPEND", bare_dir_right_append)
  V("RIGHT_TRUNCATE", bare_dir_right_truncate)
  V("RIGHT_SET_TIMES", bare_dir_right_set_times)

  V("OPEN_READ", bare_dir_open_read)
  V("OPEN_WRITE", bare_dir_open_write)
  V("OPEN_APPEND", bare_dir_open_append)
  V("OPEN_CREATE", bare_dir_open_create)
  V("OPEN_EXCLUSIVE", bare_dir_open_exclusive)
  V("OPEN_TRUNCATE", bare_dir_open_truncate)

  V("TYPE_FILE", bare_dir_type_file)
  V("TYPE_DIRECTORY", bare_dir_type_directory)
  V("TYPE_SYMLINK", bare_dir_type_symlink)
  V("TYPE_OTHER", bare_dir_type_other)
#undef V

  return exports;
}

BARE_MODULE(bare_dir, bare_dir_exports)
