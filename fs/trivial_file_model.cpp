#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "fs/trivial_file_model.h"

void fs::trivial_file_model::close()
{
  _M_read_only = true;

  if (_M_data != MAP_FAILED) {
    munmap(_M_data, _M_filesize);
    _M_data = MAP_FAILED;
  }

  if (_M_fd != -1) {
    ::close(_M_fd);
    _M_fd = -1;
  }
}

bool fs::trivial_file_model::open(const char* filename)
{
  // If the length of the file name is too long...
  size_t len;
  if ((len = strlen(filename)) >= sizeof(_M_filename)) {
    return false;
  }

  // Open file for reading / writing.
  if ((_M_fd = ::open(filename, O_RDWR)) < 0) {
    return false;
  }

  // Get file status.
  struct stat sbuf;
  if (fstat(_M_fd, &sbuf) < 0) {
    return false;
  }

  // Regular file?
  if (S_ISREG(sbuf.st_mode)) {
    _M_block_device = false;

    _M_filesize = sbuf.st_size;
  } else if (S_ISBLK(sbuf.st_mode)) {
    _M_block_device = true;

    // Get size.
    if (ioctl(_M_fd, BLKGETSIZE64, &_M_filesize) < 0) {
      return false;
    }
  } else {
    return false;
  }

  // If the file is not empty...
  if (_M_filesize != 0) {
    // Map file into memory.
    if ((_M_data = mmap(NULL,
                        _M_filesize,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        _M_fd,
                        0)) == MAP_FAILED) {
      return false;
    }
  }

  if (filename != _M_filename) {
    // Save file name.
    memcpy(_M_filename, filename, len);
    _M_filename[len] = 0;
  }

  _M_read_only = false;

  return true;
}

bool fs::trivial_file_model::modify(uint64_t off,
                                    const void* data,
                                    uint64_t len)
{
  // Read only mode?
  if (_M_read_only) {
    return false;
  }

  // If the end of the operation is beyond the end of the file...
  if (off + len > _M_filesize) {
    return false;
  }

  // Nothing to modify?
  if (len == 0) {
    return true;
  }

  // Write.
  if (pwrite(_M_fd, data, len, off) != len) {
    return false;
  }

  close();

  return open(_M_filename);
}

bool fs::trivial_file_model::add(uint64_t off, const void* data, uint64_t len)
{
  // Read only mode?
  if (_M_read_only) {
    return false;
  }

  // Block device?
  if (_M_block_device) {
    return false;
  }

  // Nothing to add?
  if (len == 0) {
    return true;
  }

  char tmpfilename[PATH_MAX];
  snprintf(tmpfilename, sizeof(tmpfilename), "%s.tmp", _M_filename);

  // Open file for writing.
  int fd;
  if ((fd = ::open(tmpfilename, O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0) {
    return false;
  }

  // Write.
  if ((pwrite(fd, _M_data, off, 0) != off) ||
      (pwrite(fd, data, len, off) != len) ||
      (pwrite(fd,
              reinterpret_cast<uint8_t*>(_M_data) + off,
              _M_filesize - off,
              off + len) != _M_filesize - off)) {
    ::close(fd);
    ::remove(tmpfilename);

    return false;
  }

  ::close(fd);
  close();

  rename(tmpfilename, _M_filename);

  return open(_M_filename);
}

bool fs::trivial_file_model::remove(uint64_t off, uint64_t len)
{
  // Read only mode?
  if (_M_read_only) {
    return false;
  }

  // Block device?
  if (_M_block_device) {
    return false;
  }

  // Nothing to delete?
  if (len == 0) {
    return true;
  }

  char tmpfilename[PATH_MAX];
  snprintf(tmpfilename, sizeof(tmpfilename), "%s.tmp", _M_filename);

  // Open file for writing.
  int fd;
  if ((fd = ::open(tmpfilename, O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0) {
    return false;
  }

  // Write.
  if ((pwrite(fd, _M_data, off, 0) != off) ||
      (pwrite(fd,
              reinterpret_cast<uint8_t*>(_M_data) + off + len,
              _M_filesize - (off + len),
              off) != _M_filesize - (off + len))) {
    ::close(fd);
    ::remove(tmpfilename);

    return false;
  }

  ::close(fd);
  close();

  rename(tmpfilename, _M_filename);

  return open(_M_filename);
}

bool fs::trivial_file_model::get(uint64_t off, void* data, uint64_t& len) const
{
  if (off >= _M_filesize) {
    return false;
  }

  if (off + len > _M_filesize) {
    len = _M_filesize - off;
  }

  memcpy(data, reinterpret_cast<uint8_t*>(_M_data) + off, len);

  return true;
}

bool fs::trivial_file_model::find_forward(uint64_t off,
                                          const void* needle,
                                          uint64_t needlelen,
                                          uint64_t& position) const
{
  if (off + needlelen > _M_filesize) {
    return false;
  }

  if (needlelen == 0) {
    return false;
  }

  const uint8_t* p;
  if ((p = reinterpret_cast<const uint8_t*>(
             memmem(reinterpret_cast<const uint8_t*>(_M_data) + off,
                    _M_filesize - off,
                    needle,
                    needlelen)
           )) != NULL) {
    position = p - reinterpret_cast<const uint8_t*>(_M_data);
    return true;
  }

  return false;
}

bool fs::trivial_file_model::find_backward(uint64_t off,
                                           const void* needle,
                                           uint64_t needlelen,
                                           uint64_t& position) const
{
  if (needlelen > _M_filesize) {
    return false;
  }

  if (needlelen == 0) {
    return false;
  }

  if (off + needlelen > _M_filesize) {
    off = _M_filesize - needlelen;
  }

  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(_M_data) + off;
       p >= _M_data;
       p--) {
    if (memcmp(p, needle, needlelen) == 0) {
      position = p - reinterpret_cast<const uint8_t*>(_M_data);
      return true;
    }
  }

  return false;
}

uint64_t fs::trivial_file_model::pwrite(int fd,
                                        const void* buf,
                                        uint64_t len,
                                        uint64_t offset)
{
  static const uint64_t kMaxWrite = 1024ull * 1024ull * 1024ull;

  uint64_t written = 0;

  while (written < len) {
    uint64_t n = len - written;
    if (n > kMaxWrite) {
      n = kMaxWrite;
    }

    ssize_t ret;
    if ((ret = ::pwrite(fd, buf, n, offset)) < 0) {
      return written;
    } else if (ret > 0) {
      buf = reinterpret_cast<const uint8_t*>(buf) + ret;
      offset += ret;
      written += ret;
    }
  }

  return written;
}
