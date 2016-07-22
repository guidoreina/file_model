#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "fs/file_change.h"

void fs::file_changes::clear()
{
  if (_M_changes) {
    for (size_t i = 0; i < _M_used; i++) {
      if (_M_changes[i].olddata) {
        free(_M_changes[i].olddata);
      }

      if (_M_changes[i].newdata) {
        free(_M_changes[i].newdata);
      }
    }

    free(_M_changes);
    _M_changes = NULL;
  }

  _M_size = 0;
  _M_used = 0;
}

bool fs::file_changes::load(const char* filename)
{
  // If the file doesn't exist or is not a regular file...
  struct stat sbuf;
  if ((stat(filename, &sbuf) < 0) || (!S_ISREG(sbuf.st_mode))) {
    return false;
  }

  // If the file is empty...
  if (sbuf.st_size == 0) {
    return false;
  }

  // Open file for reading.
  int fd;
  if ((fd = open(filename, O_RDONLY)) < 0) {
    return false;
  }

  // Map file into memory.
  void* buf;
  if ((buf = mmap(NULL,
                  sbuf.st_size,
                  PROT_READ,
                  MAP_SHARED,
                  fd,
                  0)) == MAP_FAILED) {
    close(fd);
    return false;
  }

  const uint8_t* begin = reinterpret_cast<const uint8_t*>(buf);
  const uint8_t* end = begin + sbuf.st_size;

  size_t nline = 0;
  size_t nchanges = 0;

  do {
    nline++;

    // Search end of line.
    const uint8_t* eol;
    if ((eol = reinterpret_cast<const uint8_t*>(
                 memchr(begin, '\n', end - begin)
               )) == NULL) {
      munmap(buf, sbuf.st_size);
      close(fd);

      return false;
    }

    // If the line is too short...
    size_t linelen;
    if ((linelen = eol - begin) < 20) {
      munmap(buf, sbuf.st_size);
      close(fd);

      return false;
    }

    // First line?
    if (nline == 1) {
      if (memcmp(begin, "Number of changes: ", 19) != 0) {
        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }

      // Parse number of changes.
      begin += 19;
      size_t l = 0;
      while (begin < eol) {
        if ((*begin >= '0') && (*begin <= '9')) {
          uint64_t tmp;
          if ((tmp = (nchanges * 10) + (*begin - '0')) < nchanges) {
            munmap(buf, sbuf.st_size);
            close(fd);

            return false;
          }

          nchanges = tmp;
          l++;
        } else if ((l > 0) && (*begin == '.') && (begin + 1 == eol)) {
          break;
        } else {
          munmap(buf, sbuf.st_size);
          close(fd);

          return false;
        }

        begin++;
      }

      begin = eol + 1;
      continue;
    }

    fs::file_change::type t;

    // Get type of the file change.
    size_t skip;
    if (memcmp(begin, "Modify: ", 8) == 0) {
      t = fs::file_change::type::kModify;
      skip = 8;
    } else if (memcmp(begin, "Add: ", 5) == 0) {
      t = fs::file_change::type::kAdd;
      skip = 5;
    } else if (memcmp(begin, "Remove: ", 8) == 0) {
      t = fs::file_change::type::kRemove;
      skip = 8;
    } else {
      munmap(buf, sbuf.st_size);
      close(fd);

      return false;
    }

    begin += skip;

    // Parse offset.
    if (memcmp(begin, "offset: ", 8) != 0) {
      munmap(buf, sbuf.st_size);
      close(fd);

      return false;
    }

    begin += 8;

    uint64_t off = 0;
    size_t l = 0;
    while (begin < eol) {
      if ((*begin >= '0') && (*begin <= '9')) {
        uint64_t tmp;
        if ((tmp = (off * 10) + (*begin - '0')) < off) {
          munmap(buf, sbuf.st_size);
          close(fd);

          return false;
        }

        off = tmp;
        l++;
      } else if ((l > 0) && (*begin == ',')) {
        break;
      } else {
        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }

      begin++;
    }

    // Parse length.
    if ((begin + 10 >= eol) || (memcmp(begin, ", length: ", 10) != 0)) {
      munmap(buf, sbuf.st_size);
      close(fd);

      return false;
    }

    begin += 10;

    uint64_t len = 0;
    l = 0;
    while (begin < eol) {
      if ((*begin >= '0') && (*begin <= '9')) {
        uint64_t tmp;
        if ((tmp = (len * 10) + (*begin - '0')) < len) {
          munmap(buf, sbuf.st_size);
          close(fd);

          return false;
        }

        len = tmp;
        l++;
      } else if ((l > 0) &&
                 (*begin == '.') &&
                 (begin + 1 == eol) &&
                 (len > 0)) {
        break;
      } else {
        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }

      begin++;
    }

    begin = eol + 1;

    if (t == file_change::type::kRemove) {
      if (!remove(off, NULL, len)) {
        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }
    } else {
      if ((eol = reinterpret_cast<const uint8_t*>(
                   memchr(begin, '\n', end - begin)
                 )) == NULL) {
        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }

      if ((linelen = eol - begin) != 2 * len) {
        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }

      uint8_t* line;
      if ((line = reinterpret_cast<uint8_t*>(malloc(len))) == NULL) {
        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }

      l = 0;

      for (size_t i = 0; i < linelen; i += 2) {
        uint8_t c;
        if ((begin[i] >= '0') && (begin[i] <= '9')) {
          c = begin[i] - '0';
        } else if ((begin[i] >= 'a') && (begin[i] <= 'f')) {
          c = begin[i] - 'a' + 10;
        } else if ((begin[i] >= 'A') && (begin[i] <= 'F')) {
          c = begin[i] - 'A' + 10;
        } else {
          free(line);

          munmap(buf, sbuf.st_size);
          close(fd);

          return false;
        }

        if ((begin[i + 1] >= '0') && (begin[i + 1] <= '9')) {
          c = (c * 16) + (begin[i + 1] - '0');
        } else if ((begin[i + 1] >= 'a') && (begin[i + 1] <= 'f')) {
          c = (c * 16) + (begin[i + 1] - 'a' + 10);
        } else if ((begin[i + 1] >= 'A') && (begin[i + 1] <= 'F')) {
          c = (c * 16) + (begin[i + 1] - 'A' + 10);
        } else {
          free(line);

          munmap(buf, sbuf.st_size);
          close(fd);

          return false;
        }

        line[l++] = c;
      }

      if (!register_change(t, off, NULL, line, len)) {
        free(line);

        munmap(buf, sbuf.st_size);
        close(fd);

        return false;
      }

      free(line);

      begin = eol + 1;
    }
  } while (begin < end);

  munmap(buf, sbuf.st_size);
  close(fd);

  return (nchanges == _M_used);
}

bool fs::file_changes::save(const char* filename) const
{
  FILE* file;
  if ((file = fopen(filename, "w+")) == NULL) {
    return false;
  }

  fprintf(file, "Number of changes: %zu.\n", _M_used);

  for (size_t i = 0; i < _M_used; i++) {
    switch (_M_changes[i].t) {
      case file_change::type::kModify:
        fprintf(file,
                "Modify: offset: %llu, length: %llu.\n",
                _M_changes[i].off,
                _M_changes[i].len);

        hexdump(file, _M_changes[i].newdata, _M_changes[i].len);
        break;
      case file_change::type::kAdd:
        fprintf(file,
                "Add: offset: %llu, length: %llu.\n",
                _M_changes[i].off,
                _M_changes[i].len);

        hexdump(file, _M_changes[i].newdata, _M_changes[i].len);
        break;
      case file_change::type::kRemove:
        fprintf(file,
                "Remove: offset: %llu, length: %llu.\n",
                _M_changes[i].off,
                _M_changes[i].len);

        break;
    }
  }

  fclose(file);

  return true;
}

bool fs::file_changes::register_change(file_change::type type,
                                       uint64_t off,
                                       void* olddata,
                                       const void* newdata,
                                       uint64_t len)
{
  if (len == 0) {
    return true;
  }

  if (!allocate()) {
    return false;
  }

  struct file_change* chg = &_M_changes[_M_used];

  if (newdata) {
    if ((chg->newdata = reinterpret_cast<uint8_t*>(malloc(len))) == NULL) {
      return false;
    }

    memcpy(chg->newdata, newdata, len);
  } else {
    chg->newdata = NULL;
  }

  chg->t = type;

  chg->off = off;

  chg->olddata = reinterpret_cast<uint8_t*>(olddata);

  chg->len = len;

  _M_used++;

  return true;
}

bool fs::file_changes::erase_last_change()
{
  if (_M_used == 0) {
    return false;
  }

  if (_M_changes[_M_used - 1].olddata) {
    free(_M_changes[_M_used - 1].olddata);
  }

  if (_M_changes[_M_used - 1].newdata) {
    free(_M_changes[_M_used - 1].newdata);
  }

  _M_used--;

  return true;
}

bool fs::file_changes::erase_from_position(size_t pos)
{
  if (pos >= _M_used) {
    return false;
  }

  for (size_t i = pos; i < _M_used; i++) {
    if (_M_changes[i].olddata) {
      free(_M_changes[i].olddata);
    }

    if (_M_changes[i].newdata) {
      free(_M_changes[i].newdata);
    }
  }

  _M_used = pos;

  return true;
}

bool fs::file_changes::allocate()
{
  if (_M_used == _M_size) {
    size_t size = (_M_size == 0) ? 32 : _M_size * 2;

    struct file_change* changes;
    if ((changes = reinterpret_cast<struct file_change*>(
                     realloc(_M_changes, size * sizeof(struct file_change))
                   )) == NULL) {
      return false;
    }

    _M_changes = changes;
    _M_size = size;
  }

  return true;
}

void fs::file_changes::hexdump(FILE* file, const uint8_t* data, uint64_t len)
{
  for (uint64_t i = 0; i < len; i++, data++) {
    fprintf(file, "%02x", *data);
  }

  fprintf(file, "\n");
}
