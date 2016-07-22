#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "fs/copy.h"

bool fs::copy(const char* src, const char* dest)
{
  struct stat sbuf;
  if ((stat(src, &sbuf) < 0) || (!S_ISREG(sbuf.st_mode))) {
    return false;
  }

  // Open input file for reading.
  int infd;
  if ((infd = open(src, O_RDONLY)) < 0) {
    return false;
  }

  // Open output file for writing.
  int outfd;
  if ((outfd = open(dest, O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0) {
    close(infd);
    return false;
  }

  // If the input file is empty...
  if (sbuf.st_size == 0) {
    close(outfd);
    close(infd);

    return true;
  }

  // Map input file into memory.
  void* buf;
  if ((buf = mmap(NULL,
                  sbuf.st_size,
                  PROT_READ,
                  MAP_SHARED,
                  infd,
                  0)) == MAP_FAILED) {
    close(outfd);
    unlink(dest);

    close(infd);

    return false;
  }

  // Copy.
  size_t written = 0;
  do {
    ssize_t w;
    if ((w = write(outfd,
                   reinterpret_cast<const uint8_t*>(buf) + written,
                   sbuf.st_size - written)) < 0) {
      close(outfd);
      unlink(dest);

      munmap(buf, sbuf.st_size);
      close(infd);

      return false;
    } else if (w > 0) {
      written += w;
    }
  } while (written < static_cast<size_t>(sbuf.st_size));

  close(outfd);

  munmap(buf, sbuf.st_size);
  close(infd);

  return true;
}
