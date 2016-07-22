#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "fs/diff.h"

bool fs::diff(const char* file1, const char* file2)
{
  struct stat sbuf1;
  if ((stat(file1, &sbuf1) < 0) || (!S_ISREG(sbuf1.st_mode))) {
    return false;
  }

  struct stat sbuf2;
  if ((stat(file2, &sbuf2) < 0) || (!S_ISREG(sbuf2.st_mode))) {
    return false;
  }

  if (sbuf1.st_size != sbuf2.st_size) {
    return false;
  }

  if (sbuf1.st_size == 0) {
    return true;
  }

  int fd1;
  if ((fd1 = open(file1, O_RDONLY)) < 0) {
    return false;
  }

  int fd2;
  if ((fd2 = open(file2, O_RDONLY)) < 0) {
    close(fd1);
    return false;
  }

  void* buf1;
  if ((buf1 = mmap(NULL,
                   sbuf1.st_size,
                   PROT_READ,
                   MAP_SHARED,
                   fd1,
                   0)) == MAP_FAILED) {
    close(fd2);
    close(fd1);

    return false;
  }

  void* buf2;
  if ((buf2 = mmap(NULL,
                   sbuf2.st_size,
                   PROT_READ,
                   MAP_SHARED,
                   fd2,
                   0)) == MAP_FAILED) {
    munmap(buf1, sbuf1.st_size);

    close(fd2);
    close(fd1);

    return false;
  }

  int ret = memcmp(buf1, buf2, sbuf1.st_size);

  munmap(buf2, sbuf2.st_size);
  munmap(buf1, sbuf1.st_size);

  close(fd2);
  close(fd1);

  return (ret == 0);
}
