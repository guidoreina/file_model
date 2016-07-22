#ifndef FS_RANDOM_FILE_H
#define FS_RANDOM_FILE_H

#include <stdint.h>

namespace fs {
  bool random_file(const char* filename, uint64_t len);
}

#endif // FS_RANDOM_FILE_H
