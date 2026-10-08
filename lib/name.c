#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "dir.h"

#define BARE_DIR_NAME_MAX 255

#ifdef _WIN32

static bool
bare_dir__equals_ignore_case(const char *a, size_t len, const char *b) {
  if (strlen(b) != len) return false;

  for (size_t i = 0; i < len; i++) {
    char c = a[i];

    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';

    if (c != b[i]) return false;
  }

  return true;
}

// Win32 treats these as devices with any extension and ignores spaces before
// it, so `nul .txt` is a device too.
// https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file
static bool
bare_dir__is_reserved(const char *name, size_t len) {
  const char *dot = memchr(name, '.', len);

  if (dot) len = dot - name;

  while (len > 0 && name[len - 1] == ' ')
    len--;

  static const char *reserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"};

  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    if (bare_dir__equals_ignore_case(name, len, reserved[i])) return true;
  }

  if (len < 4) return false;

  if (!bare_dir__equals_ignore_case(name, 3, "COM") && !bare_dir__equals_ignore_case(name, 3, "LPT")) {
    return false;
  }

  const char *suffix = name + 3;
  size_t suffix_len = len - 3;

  if (suffix_len == 1 && suffix[0] >= '0' && suffix[0] <= '9') return true;

  // Superscript 1, 2 and 3 in UTF-8.
  if (suffix_len == 2 && (unsigned char) suffix[0] == 0xc2) {
    unsigned char c = (unsigned char) suffix[1];

    return c == 0xb9 || c == 0xb2 || c == 0xb3;
  }

  return false;
}

#endif

bool
bare_dir_valid_name(const char *name, size_t len) {
  if (len == 0 || len > BARE_DIR_NAME_MAX) return false;

  if (len == 1 && name[0] == '.') return false;
  if (len == 2 && name[0] == '.' && name[1] == '.') return false;

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char) name[i];

    if (c == '\0' || c == '/') return false;

#ifdef _WIN32
    if (c < 0x20) return false;

    switch (c) {
    case '\\':
    case ':':
    case '"':
    case '*':
    case '?':
    case '<':
    case '>':
    case '|':
      return false;
    }
#endif
  }

#ifdef _WIN32
  if (name[len - 1] == '.' || name[len - 1] == ' ') return false;

  if (bare_dir__is_reserved(name, len)) return false;
#endif

  return true;
}
