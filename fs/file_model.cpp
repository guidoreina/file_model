#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "fs/file_model.h"

void fs::file_model::close()
{
  _M_read_only = true;

  _M_len = 0;

  _M_memory_used = 0;

  // Free blocks.
  free_block_list(_M_header.next, &_M_header);

  _M_header.prev = &_M_header;
  _M_header.next = &_M_header;

  if (_M_data != MAP_FAILED) {
    munmap(_M_data, _M_filesize);
    _M_data = MAP_FAILED;
  }

  if (_M_fd != -1) {
    ::close(_M_fd);
    _M_fd = -1;
  }

  _M_modified = false;
  _M_size_modified = false;
}

bool fs::file_model::open(const char* filename, open_mode mode)
{
  // If the length of the file name is too long...
  size_t len;
  if ((len = strlen(filename)) >= sizeof(_M_filename)) {
    return false;
  }

  int open_flags;
  int mmap_prot;
  if (mode == open_mode::kReadWrite) {
    _M_read_only = false;

    open_flags = O_RDWR;
    mmap_prot = PROT_READ | PROT_WRITE;
  } else {
    _M_read_only = true;

    open_flags = O_RDONLY;
    mmap_prot = PROT_READ;
  }

  // Open file.
  if ((_M_fd = ::open(filename, open_flags)) < 0) {
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
                        mmap_prot,
                        MAP_SHARED,
                        _M_fd,
                        0)) == MAP_FAILED) {
      return false;
    }

    // Create block.
    struct block* b;
    if ((b = reinterpret_cast<struct block*>(
               malloc(sizeof(struct block))
             )) == NULL) {
      return false;
    }

    b->data = reinterpret_cast<uint8_t*>(_M_data);
    b->len = _M_filesize;
    b->in_memory = false;

    b->prev = &_M_header;
    b->next = &_M_header;

    _M_header.prev = b;
    _M_header.next = b;
  }

  if (filename != _M_filename) {
    // Save file name.
    memcpy(_M_filename, filename, len);
    _M_filename[len] = 0;

    if (_M_undo_enabled) {
      _M_changes.clear();
    }
  }

  _M_len = _M_filesize;

  return true;
}

bool fs::file_model::save()
{
  // If the file has not been modified...
  if (!_M_modified) {
    return true;
  }

  // If the file has neither shrinked nor grown...
  if (!_M_size_modified) {
    return save_in_place();
  }

  char tmpfilename[PATH_MAX];
  snprintf(tmpfilename, sizeof(tmpfilename), "%s.tmp", _M_filename);

  // Open file for writing.
  int fd;
  if ((fd = ::open(tmpfilename, O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0) {
    return false;
  }

  // Write blocks.
  const struct block* b = _M_header.next;
  while (b != &_M_header) {
    // Write block.
    if (write(fd, b->data, b->len) != b->len) {
      ::close(fd);
      ::remove(tmpfilename);

      return false;
    }

    b = b->next;
  }

  ::close(fd);
  close();

  rename(tmpfilename, _M_filename);

  return open(_M_filename);
}

const char* fs::file_model::operation_result_to_string(operation_result res)
{
  switch (res) {
    case operation_result::kErrorReadOnly:
      return "kErrorReadOnly";
    case operation_result::kErrorBlockDevice:
      return "kErrorBlockDevice";
    case operation_result::kInvalidOperation:
      return "kInvalidOperation";
    case operation_result::kChangeBiggerMaxMemoryUsed:
      return "kChangeBiggerMaxMemoryUsed";
    case operation_result::kNoMemory:
      return "kNoMemory";
    case operation_result::kErrorNeedSave:
      return "kErrorNeedSave";
    case operation_result::kErrorUndoDisabled:
      return "kErrorUndoDisabled";
    case operation_result::kNoMoreChanges:
      return "kNoMoreChanges";
    case operation_result::kSuccess:
      return "kSuccess";
    default:
      return "(unknown)";
  }
}

fs::file_model::operation_result fs::file_model::modify(uint64_t off,
                                                        const void* data,
                                                        uint64_t len,
                                                        bool record_change)
{
  // Read only mode?
  if (_M_read_only) {
    return operation_result::kErrorReadOnly;
  }

  // If the end of the operation is beyond the end of the file...
  if (off + len > _M_len) {
    return operation_result::kInvalidOperation;
  }

  // If the change is bigger than the maximum memory which can be used...
  if (len > kMaxMemoryUsed) {
    return operation_result::kChangeBiggerMaxMemoryUsed;
  }

  // Seek to offset.
  struct block* b;
  uint64_t pos;
  if (!seek(off, b, pos)) {
    // File is empty.
    return operation_result::kInvalidOperation;
  }

  // Nothing to modify?
  if (len == 0) {
    return operation_result::kSuccess;
  }

  // Too many changes already?
  if (_M_memory_used + len > kMaxMemoryUsed) {
    return operation_result::kErrorNeedSave;
  }

  // If undo is enabled and the change should be recorded...
  if ((record_change &= _M_undo_enabled) == true) {
    // Get data to be replaced.
    uint8_t* olddata;
    if ((olddata = reinterpret_cast<uint8_t*>(malloc(len))) == NULL) {
      return operation_result::kNoMemory;
    }

    size_t l = len;
    get(b, pos, olddata, l);

    _M_changes.erase_from_position(_M_nchange);

    // Record change.
    if (!_M_changes.modify(off, olddata, data, l)) {
      free(olddata);
      return operation_result::kNoMemory;
    }
  }

  do {
    struct block* nextblk;

    // If the block is in disk...
    if (!b->in_memory) {
      uint8_t* buf;
      if ((buf = reinterpret_cast<uint8_t*>(
                   malloc(kMemoryBlockSize)
                 )) == NULL) {
        if (record_change) {
          _M_changes.erase_last_change();
        }

        return operation_result::kNoMemory;
      }

      // Copy half a block before the offset (it might be modified later).
      uint64_t begin;
      uint64_t count;
      if (pos <= kMidMemoryBlock) {
        begin = 0;

        if (pos > 0) {
          count = pos;
          memcpy(buf, b->data, count);
        } else {
          count = 0;
        }
      } else {
        begin = pos - kMidMemoryBlock;

        count = kMidMemoryBlock;
        memcpy(buf, b->data + begin, count);
      }

      // Left in block in memory.
      uint64_t left_memory_block = kMemoryBlockSize - count;

      uint64_t l = (len < left_memory_block) ? len : left_memory_block;
      if (pos + l > b->len) {
        l = b->len - pos;
      }

      // Copy from user's buffer.
      memcpy(buf + count, data, l);
      data = reinterpret_cast<const uint8_t*>(data) + l;

      count += l;
      left_memory_block -= l;

      // If we have copied all the user's buffer and there is still space...
      if (((len -= l) == 0) && (left_memory_block > 0)) {
        uint64_t end = pos + l;

        if (end < b->len) {
          // Left in block in disk.
          uint64_t left_disk_block = b->len - end;

          l = (left_memory_block < left_disk_block) ? left_memory_block :
                                                      left_disk_block;

          memcpy(buf + count, b->data + end, l);
          count += l;
        }
      }

      // If the new block should be inserted before the current block...
      if (begin == 0) {
        // If the new block can replace the old block...
        if (count == b->len) {
          b->data = buf;
          b->in_memory = true;

          nextblk = b->next;
        } else {
          // Create new block in memory.
          struct block* memblk;
          if ((memblk = reinterpret_cast<struct block*>(
                          malloc(sizeof(struct block))
                        )) == NULL) {
            free(buf);

            if (record_change) {
              _M_changes.erase_last_change();
            }

            return operation_result::kNoMemory;
          }

          memblk->data = buf;
          memblk->len = count;

          memblk->in_memory = true;

          b->data += count;
          b->len -= count;

          memblk->prev = b->prev;
          memblk->prev->next = memblk;

          memblk->next = b;
          b->prev = memblk;

          nextblk = b;
        }
      } else {
        // Create new block in memory.
        struct block* memblk;
        if ((memblk = reinterpret_cast<struct block*>(
                        malloc(sizeof(struct block))
                      )) == NULL) {
          free(buf);

          if (record_change) {
            _M_changes.erase_last_change();
          }

          return operation_result::kNoMemory;
        }

        memblk->data = buf;
        memblk->len = count;

        memblk->in_memory = true;

        // If the end of the block in disk is not contained in the block
        // in memory...
        uint64_t end;
        if ((end = begin + count) < b->len) {
          // Create new block in disk.
          struct block* diskblk;
          if ((diskblk = reinterpret_cast<struct block*>(
                           malloc(sizeof(struct block))
                         )) == NULL) {
            free(memblk);
            free(buf);

            if (record_change) {
              _M_changes.erase_last_change();
            }

            return operation_result::kNoMemory;
          }

          diskblk->data = b->data + end;
          diskblk->len = b->len - end;

          diskblk->in_memory = false;

          diskblk->next = b->next;
          diskblk->next->prev = diskblk;

          memblk->next = diskblk;
          diskblk->prev = memblk;

          nextblk = diskblk;
        } else {
          memblk->next = b->next;
          memblk->next->prev = memblk;

          nextblk = memblk->next;
        }

        b->len = begin;

        memblk->prev = b;
        b->next = memblk;
      }

      b = nextblk;

      _M_memory_used += kMemoryBlockSize;
    } else {
      // The block is in memory.

      // Left in block in memory.
      uint64_t left_memory_block = b->len - pos;

      uint64_t l = (len < left_memory_block) ? len : left_memory_block;

      // Copy from user's buffer.
      memcpy(b->data + pos, data, l);

      data = reinterpret_cast<const uint8_t*>(data) + l;
      len -= l;

      b = b->next;
    }

    pos = 0;
  } while (len > 0);

  _M_modified = true;

  if (record_change) {
    _M_nchange++;
  }

  return operation_result::kSuccess;
}

fs::file_model::operation_result fs::file_model::add(uint64_t off,
                                                     const void* data,
                                                     uint64_t len,
                                                     bool record_change)
{
  // Read only mode?
  if (_M_read_only) {
    return operation_result::kErrorReadOnly;
  }

  // Block device?
  if (_M_block_device) {
    return operation_result::kErrorBlockDevice;
  }

  // If the change is bigger than the maximum memory which can be used...
  if (len > kMaxMemoryUsed) {
    return operation_result::kChangeBiggerMaxMemoryUsed;
  }

  // Seek to offset.
  struct block* b;
  uint64_t pos;
  if (!seek(off, b, pos)) {
    // If the offset is at the end of the file...
    if (off == _M_len) {
      // Get pointer to last block.
      b = _M_header.prev;

      pos = b->len;
    } else {
      return operation_result::kInvalidOperation;
    }
  }

  // Nothing to modify?
  if (len == 0) {
    return operation_result::kSuccess;
  }

  // Too many changes already?
  if (_M_memory_used + len > kMaxMemoryUsed) {
    return operation_result::kErrorNeedSave;
  }

  // If undo is enabled and the change should be recorded...
  if ((record_change &= _M_undo_enabled) == true) {
    _M_changes.erase_from_position(_M_nchange);

    // Record change.
    if (!_M_changes.add(off, data, len)) {
      return operation_result::kNoMemory;
    }
  }

  // If the block is in memory...
  if (b->in_memory) {
    // Left in block in memory.
    uint64_t left_memory_block = kMemoryBlockSize - b->len;

    // If it fits...
    if (len <= left_memory_block) {
      uint64_t n = b->len - pos;
      if (n > 0) {
        memmove(b->data + pos + len, b->data + pos, n);
      }

      // Copy from user's buffer.
      memcpy(b->data + pos, data, len);
      b->len += len;

      _M_len += len;

      _M_modified = true;
      _M_size_modified = true;

      if (record_change) {
        _M_nchange++;
      }

      return operation_result::kSuccess;
    } else if ((off == _M_len) && (left_memory_block > 0)) {
      // Copy from user's buffer.
      memcpy(b->data + pos, data, left_memory_block);
      b->len += left_memory_block;

      data = reinterpret_cast<const uint8_t*>(data) + left_memory_block;
      len -= left_memory_block;

      _M_len += left_memory_block;
      off += left_memory_block;

      _M_modified = true;
      _M_size_modified = true;
    }
  }

  struct block* first, *last;
  size_t nblocks;
  if (!add(reinterpret_cast<const uint8_t*>(data), len, first, last, nblocks)) {
    if (record_change) {
      _M_changes.erase_last_change();
    }

    return operation_result::kNoMemory;
  }

  // If the blocks should be inserted before the current block...
  if (pos == 0) {
    first->prev = b->prev;
    first->prev->next = first;

    last->next = b;
    b->prev = last;
  } else if (off != _M_len) {
    // Not at the end of the file.

    uint8_t* buf;
    uint64_t l = b->len - pos;

    if (b->in_memory) {
      if ((buf = reinterpret_cast<uint8_t*>(
                   malloc(kMemoryBlockSize)
                 )) == NULL) {
        free_block_list(first, NULL);

        if (record_change) {
          _M_changes.erase_last_change();
        }

        return operation_result::kNoMemory;
      }

      memcpy(buf, b->data + pos, l);

      nblocks++;
    } else {
      buf = b->data + pos;
    }

    // Create new block.
    struct block* blk;
    if ((blk = reinterpret_cast<struct block*>(
                 malloc(sizeof(struct block))
               )) == NULL) {
      if (b->in_memory) {
        free(buf);
      }

      free_block_list(first, NULL);

      if (record_change) {
        _M_changes.erase_last_change();
      }

      return operation_result::kNoMemory;
    }

    blk->data = buf;
    blk->len = l;

    b->len -= l;

    blk->in_memory = b->in_memory;

    blk->prev = last;
    last->next = blk;

    blk->next = b->next;
    blk->next->prev = blk;

    b->next = first;
    first->prev = b;
  } else {
    first->prev = b;
    b->next = first;

    last->next = &_M_header;
    _M_header.prev = last;
  }

  _M_len += len;
  _M_memory_used += (nblocks * kMemoryBlockSize);

  _M_modified = true;
  _M_size_modified = true;

  if (record_change) {
    _M_nchange++;
  }

  return operation_result::kSuccess;
}

fs::file_model::operation_result fs::file_model::remove(uint64_t off,
                                                        uint64_t len,
                                                        bool record_change)
{
  // Read only mode?
  if (_M_read_only) {
    return operation_result::kErrorReadOnly;
  }

  // Block device?
  if (_M_block_device) {
    return operation_result::kErrorBlockDevice;
  }

  // Seek to offset.
  struct block* b;
  uint64_t pos;
  if (!seek(off, b, pos)) {
    return operation_result::kInvalidOperation;
  }

  // Nothing to modify?
  if (len == 0) {
    return operation_result::kSuccess;
  }

  // If undo is enabled and the change should be recorded...
  if ((record_change &= _M_undo_enabled) == true) {
    // Get data to be removed.
    uint8_t* olddata;
    if ((olddata = reinterpret_cast<uint8_t*>(malloc(len))) == NULL) {
      return operation_result::kNoMemory;
    }

    size_t l = len;
    get(b, pos, olddata, l);

    _M_changes.erase_from_position(_M_nchange);

    // Record change.
    if (!_M_changes.remove(off, olddata, l)) {
      free(olddata);
      return operation_result::kNoMemory;
    }
  }

  if (off + len > _M_len) {
    len = _M_len - off;
  }

  // If the change is inside the block...
  uint64_t n = pos + len;
  if (n < b->len) {
    // If the block is in disk...
    if (!b->in_memory) {
      // If not at the beginning of the block...
      if (pos != 0) {
        // Create new block in disk.
        struct block* diskblk;
        if ((diskblk = reinterpret_cast<struct block*>(
                         malloc(sizeof(struct block))
                       )) == NULL) {
          if (record_change) {
            _M_changes.erase_last_change();
          }

          return operation_result::kNoMemory;
        }

        diskblk->data = b->data + n;
        diskblk->len = b->len - n;

        b->len = pos;

        diskblk->in_memory = false;

        diskblk->prev = b;

        diskblk->next = b->next;
        diskblk->next->prev = diskblk;

        b->next = diskblk;
      } else {
        b->data += len;
        b->len -= len;
      }
    } else {
      // The block is in memory.
      memmove(b->data + pos, b->data + n, b->len - n);
      b->len -= len;
    }

    _M_len -= len;

    _M_modified = true;
    _M_size_modified = true;

    if (record_change) {
      _M_nchange++;
    }

    return operation_result::kSuccess;
  } else if (n == b->len) {
    b->len = pos;
    _M_len -= len;

    _M_modified = true;
    _M_size_modified = true;

    if (record_change) {
      _M_nchange++;
    }

    return operation_result::kSuccess;
  }

  _M_len -= len;

  // If not at the beginning of the block...
  if (pos != 0) {
    len -= (b->len - pos);

    b->len = pos;

    b = b->next;
  }

  struct block* prev = b->prev;

  do {
    if (len >= b->len) {
      struct block* next = b->next;

      len -= b->len;

      // If the data is in memory...
      if (b->in_memory) {
        free(b->data);
        _M_memory_used -= kMemoryBlockSize;
      }

      free(b);

      b = next;
    } else {
      // If the block is in disk...
      if (!b->in_memory) {
        b->data += len;
      } else {
        memmove(b->data, b->data + len, b->len - len);
      }

      b->len -= len;

      break;
    }
  } while (len > 0);

  prev->next = b;
  b->prev = prev;

  _M_modified = true;
  _M_size_modified = true;

  if (record_change) {
    _M_nchange++;
  }

  return operation_result::kSuccess;
}

fs::file_model::operation_result fs::file_model::undo()
{
  // Read only mode?
  if (_M_read_only) {
    return operation_result::kErrorReadOnly;
  }

  if (!_M_undo_enabled) {
    return operation_result::kErrorUndoDisabled;
  }

  if (_M_nchange == 0) {
    return operation_result::kNoMoreChanges;
  }

  const struct file_change* chg = _M_changes.get(_M_nchange - 1);

  operation_result res;

  switch (chg->t) {
    case file_change::type::kModify:
      res = modify(chg->off, chg->olddata, chg->len, false);
      break;
    case file_change::type::kAdd:
      res = remove(chg->off, chg->len, false);
      break;
    default: // file_change::type::kRemove.
      res = add(chg->off, chg->olddata, chg->len, false);
      break;
  }

  if (res == operation_result::kSuccess) {
    _M_nchange--;
    return operation_result::kSuccess;
  } else {
    return res;
  }
}

fs::file_model::operation_result fs::file_model::redo()
{
  // Read only mode?
  if (_M_read_only) {
    return operation_result::kErrorReadOnly;
  }

  if (!_M_undo_enabled) {
    return operation_result::kErrorUndoDisabled;
  }

  if (_M_nchange == _M_changes.size()) {
    return operation_result::kNoMoreChanges;
  }

  const struct file_change* chg = _M_changes.get(_M_nchange);

  operation_result res;

  switch (chg->t) {
    case file_change::type::kModify:
      res = modify(chg->off, chg->newdata, chg->len, false);
      break;
    case file_change::type::kAdd:
      res = add(chg->off, chg->newdata, chg->len, false);
      break;
    default: // file_change::type::kRemove.
      res = remove(chg->off, chg->len, false);
      break;
  }

  if (res == operation_result::kSuccess) {
    _M_nchange++;
    return operation_result::kSuccess;
  } else {
    return res;
  }
}

bool fs::file_model::get(uint64_t off, void* data, uint64_t& len) const
{
  // Seek to offset.
  const struct block* b;
  uint64_t pos;
  if (!seek(off, b, pos)) {
    return false;
  }

  get(b, pos, data, len);

  return true;
}

bool fs::file_model::save_in_place()
{
  // Write blocks.
  uint64_t off = 0;
  const struct block* b = _M_header.next;
  while (b != &_M_header) {
    // If the block is in memory...
    if (b->in_memory) {
      // Seek.
      if (lseek(_M_fd, off, SEEK_SET) != static_cast<off_t>(off)) {
        return false;
      }

      // Write block.
      if (write(_M_fd, b->data, b->len) != b->len) {
        return false;
      }
    }

    off += b->len;

    b = b->next;
  }

  close();

  return open(_M_filename);
}

void fs::file_model::get(const struct block* b,
                         uint64_t pos,
                         void* data,
                         uint64_t& len) const
{
  uint64_t left;
  if ((left = len) == 0) {
    return;
  }

  uint64_t written = 0;

  do {
    uint64_t count = b->len - pos;
    if (count >= left) {
      memcpy(data, b->data + pos, left);
      len = written + left;

      return;
    }

    memcpy(data, b->data + pos, count);
    data = reinterpret_cast<uint8_t*>(data) + count;
    written += count;
    left -= count;

    b = b->next;
    pos = 0;
  } while (b != &_M_header);

  len = written;
}

bool fs::file_model::seek(uint64_t off,
                          const struct block*& b,
                          uint64_t& pos) const
{
  // If the offset is beyond the end of file...
  if (off >= _M_len) {
    return false;
  }

  uint64_t n = 0;

  // Search block which contains the offset 'off'.
  const struct block* blk = _M_header.next;
  do {
    uint64_t next = n + blk->len;

    if (off < next) {
      b = blk;
      pos = off - n;

      return true;
    }

    n = next;
  } while ((blk = blk->next) != &_M_header);

  return false;
}

bool fs::file_model::find_forward(uint64_t off,
                                  const void* needle,
                                  uint64_t needlelen,
                                  uint64_t& position) const
{
  if (off + needlelen > _M_len) {
    return false;
  }

  if (needlelen == 0) {
    return false;
  }

  // Seek to offset.
  const struct block* b;
  uint64_t pos;
  if (!seek(off, b, pos)) {
    return false;
  }

  // Make 'off' point to the beginning of the block.
  off -= pos;

  do {
    // If the needle fits in the current block...
    if (pos + needlelen <= b->len) {
      const uint8_t* p;
      if ((p = reinterpret_cast<const uint8_t*>(memmem(b->data + pos,
                                                       b->len - pos,
                                                       needle,
                                                       needlelen))) != NULL) {
        position = off + (p - b->data);
        return true;
      }

      pos = b->len - needlelen + 1;
    }

    const struct block* next;
    if ((next = b->next) == &_M_header) {
      return false;
    }

    for (uint64_t left = b->len - pos; left > 0; left--, pos++) {
      if (memcmp(b->data + pos, needle, left) == 0) {
        uint64_t l = needlelen - left;
        uint64_t idx = left;

        do {
          if (l <= next->len) {
            if (memcmp(next->data,
                       reinterpret_cast<const uint8_t*>(needle) + idx,
                       l) == 0) {
              position = off + pos;
              return true;
            }

            break;
          } else {
            if (memcmp(next->data,
                       reinterpret_cast<const uint8_t*>(needle) + idx,
                       next->len) == 0) {
              idx += next->len;
              l -= next->len;

              if ((next = next->next) == &_M_header) {
                return false;
              }
            } else {
              break;
            }
          }
        } while (true);
      }
    }

    off += b->len;

    b = b->next;
    pos = 0;
  } while (true);
}

bool fs::file_model::find_backward(uint64_t off,
                                   const void* needle,
                                   uint64_t needlelen,
                                   uint64_t& position) const
{
  if (needlelen > _M_len) {
    return false;
  }

  if (needlelen == 0) {
    return false;
  }

  // Seek to offset.
  const struct block* b;
  uint64_t pos;

  if (off + needlelen >= _M_len) {
    b = _M_header.prev;
    pos = b->len;

    // Make 'off' point to the beginning of the block.
    off = _M_len - b->len;
  } else {
    off += needlelen;

    if (!seek(off, b, pos)) {
      return false;
    }

    // Make 'off' point to the beginning of the block.
    off -= pos;
  }

  do {
    // If the needle fits in the current block...
    if (needlelen <= pos) {
      for (const uint8_t* p = b->data + pos - needlelen; p >= b->data; p--) {
        if (memcmp(p, needle, needlelen) == 0) {
          position = off + (p - b->data);
          return true;
        }
      }

      pos = needlelen - 1;
    }

    const struct block* prev;
    if ((prev = b->prev) == &_M_header) {
      return false;
    }

    off -= prev->len;

    for (uint64_t left = pos; left > 0; left--) {
      uint64_t l = needlelen - left;
      if (memcmp(b->data,
                 reinterpret_cast<const uint8_t*>(needle) + l,
                 left) == 0) {
        uint64_t tmpoff = off;

        do {
          if (l <= prev->len) {
            uint64_t idx = prev->len - l;

            if (memcmp(prev->data + idx, needle, l) == 0) {
              position = tmpoff + idx;
              return true;
            }

            break;
          } else {
            if (memcmp(prev->data,
                       reinterpret_cast<const uint8_t*>(needle) +
                       (l - prev->len),
                       prev->len) == 0) {
              l -= prev->len;

              if ((prev = prev->prev) == &_M_header) {
                return false;
              }

              tmpoff -= prev->len;
            } else {
              break;
            }
          }
        } while (true);
      }
    }

    b = b->prev;
    pos = b->len;
  } while (true);
}

bool fs::file_model::add(const uint8_t* data,
                         uint64_t len,
                         struct block*& first,
                         struct block*& last,
                         size_t& nblocks)
{
  struct block* header = NULL;
  struct block* prev = NULL;

  size_t count = 0;

  while (len > 0) {
    uint8_t* buf;
    if ((buf = reinterpret_cast<uint8_t*>(malloc(kMemoryBlockSize))) == NULL) {
      free_block_list(header, NULL);
      return false;
    }

    struct block* b;
    if ((b = reinterpret_cast<struct block*>(
               malloc(sizeof(struct block))
             )) == NULL) {
      free(buf);
      free_block_list(header, NULL);

      return false;
    }

    uint64_t l = (len < kMemoryBlockSize) ? len : kMemoryBlockSize;

    memcpy(buf, data, l);
    data += l;
    len -= l;

    b->data = buf;
    b->len = l;

    b->in_memory = true;

    b->prev = prev;
    b->next = NULL;

    if (prev) {
      prev->next = b;
    } else {
      header = b;
    }

    prev = b;

    count++;
  }

  first = header;
  last = prev;

  nblocks = count;

  return true;
}

void fs::file_model::free_block_list(struct block* begin,
                                     const struct block* end)
{
  while (begin != end) {
    struct block* next = begin->next;

    // If the data is in memory...
    if (begin->in_memory) {
      free(begin->data);
    }

    free(begin);

    begin = next;
  }
}

uint64_t fs::file_model::write(int fd, const void* buf, uint64_t len)
{
  static const uint64_t kMaxWrite = 1024ull * 1024ull * 1024ull;

  uint64_t written = 0;

  while (written < len) {
    uint64_t n = len - written;
    if (n > kMaxWrite) {
      n = kMaxWrite;
    }

    ssize_t ret;
    if ((ret = ::write(fd, buf, n)) < 0) {
      return written;
    } else if (ret > 0) {
      buf = reinterpret_cast<const uint8_t*>(buf) + ret;
      written += ret;
    }
  }

  return written;
}
