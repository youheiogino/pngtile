#include "path.h"
#include "pngtile.h"

#include <string.h>
#include <errno.h>

const char *pt_path_ext(const char *path)
{
  const char *ext, *p;

  for (p = path; *p; p++) {
    if (*p == '/') {
      // ignore dots in directory names
      ext = NULL;
    } else if (*p == '.') {
      ext = p;
    }
  }

  if (ext == NULL) {
    // pointer to end of string
    ext = p;
  }

  return ext;
}

int pt_path_make_ext (char *buf, size_t len, const char *path, const char *ext)
{
    char *p;

    if (strlen(path) >= len) {
        return -PT_ERR_PATH;
    }

    strcpy(buf, path);

    // find .ext
    if ((p = strrchr(buf, '.')) == NULL) {
        // TODO: seek to end instead?
        return -PT_ERR_PATH;
    }

    // check length
    if (p + strlen(ext) >= buf + len) {
        return -PT_ERR_PATH;
    }

    // change to .foo
    strcpy(p, ext);

    // ok
    return 0;
}
