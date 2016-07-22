#ifndef FS_FILE_MODEL_H
#define FS_FILE_MODEL_H

#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <limits.h>
#include "fs/file_change.h"
#include "types/direction.h"

namespace fs {
  class file_model {
    public:
      // Constructor.
      file_model(bool undo_enabled = true);

      // Destructor.
      ~file_model();

      // Open.
      enum class open_mode {
        kReadWrite,
        kReadOnly
      };

      bool open(const char* filename,
                open_mode mode = open_mode::kReadWrite);

      // Close.
      void close();

      // Save.
      bool save();

      enum class operation_result {
        kErrorReadOnly,
        kErrorBlockDevice,
        kInvalidOperation,
        kChangeBiggerMaxMemoryUsed,
        kNoMemory,
        kErrorNeedSave,
        kErrorUndoDisabled,
        kNoMoreChanges,
        kSuccess
      };

      static const char* operation_result_to_string(operation_result res);

      // Modify.
      operation_result modify(uint64_t off,
                              const void* data,
                              uint64_t len,
                              bool record_change = true);

      // Add.
      operation_result add(uint64_t off,
                           const void* data,
                           uint64_t len,
                           bool record_change = true);

      // Remove.
      operation_result remove(uint64_t off,
                              uint64_t len,
                              bool record_change = true);

      // Undo.
      operation_result undo();

      // Redo.
      operation_result redo();

      // Get data.
      bool get(uint64_t off, void* data, uint64_t& len) const;

      // Find.
      bool find(uint64_t off,
                direction dir,
                const void* needle,
                uint64_t needlelen,
                uint64_t& position) const;

      // Read only mode?
      bool read_only() const;

      // Is block device?
      bool block_device() const;

      // Get length.
      uint64_t length() const;

      // Get memory used.
      uint64_t memory_used() const;

      // Has the file been modified?
      bool modified() const;

    private:
      static const uint64_t kMemoryBlockSize = 4 * 1024;
      static const uint64_t kMidMemoryBlock = kMemoryBlockSize / 2;
      static const uint64_t kMaxMemoryUsed = 100 * 1024 * 1024;

      // Undo enabled?
      bool _M_undo_enabled;

      // Changes.
      file_changes _M_changes;
      size_t _M_nchange;

      // File name.
      char _M_filename[PATH_MAX];

      // File descriptor.
      int _M_fd;

      // Read only mode?
      bool _M_read_only;

      // Block device?
      bool _M_block_device;

      // File size.
      uint64_t _M_filesize;

      // Pointer to memory mapped file.
      void* _M_data;

      // Current length.
      uint64_t _M_len;

      // Memory used.
      uint64_t _M_memory_used;

      struct block {
        // Block data:
        //   It points to one of the following two locations:
        //     - Somewhere in [_M_data, _M_data + _M_filesize)
        //       if in_memory = false
        //     - An allocated buffer if in_memory = true
        uint8_t* data;

        // Block length.
        uint64_t len;

        // Is the block in memory or in disk?
        bool in_memory;

        struct block* prev;
        struct block* next;
      };

      block _M_header;

      // Has the file been modified?
      bool _M_modified;

      // Has the file been shrinked or grown?
      bool _M_size_modified;

      // Save file in-place.
      bool save_in_place();

      // Get data.
      void get(const struct block* b,
               uint64_t pos,
               void* data,
               uint64_t& len) const;

      // Seek.
      bool seek(uint64_t off, const struct block*& b, uint64_t& pos) const;
      bool seek(uint64_t off, struct block*& b, uint64_t& pos) const;

      // Find forward.
      bool find_forward(uint64_t off,
                        const void* needle,
                        uint64_t needlelen,
                        uint64_t& position) const;

      // Find backward.
      bool find_backward(uint64_t off,
                         const void* needle,
                         uint64_t needlelen,
                         uint64_t& position) const;

      // Add.
      static bool add(const uint8_t* data,
                      uint64_t len,
                      struct block*& first,
                      struct block*& last,
                      size_t& nblocks);

      // Free block list.
      static void free_block_list(struct block* begin, const struct block* end);

      // Write.
      static uint64_t write(int fd, const void* buf, uint64_t len);

      // Disable copy constructor and assignment operator.
      file_model(const file_model&) = delete;
      file_model& operator=(const file_model&) = delete;
  };

  inline file_model::file_model(bool undo_enabled)
    : _M_undo_enabled(undo_enabled),
      _M_nchange(0),
      _M_fd(-1),
      _M_read_only(true),
      _M_block_device(false),
      _M_filesize(0),
      _M_data(MAP_FAILED),
      _M_len(0),
      _M_memory_used(0),
      _M_modified(false),
      _M_size_modified(false)
  {
    *_M_filename = 0;

    _M_header.len = 0;
    _M_header.in_memory = false;

    _M_header.prev = &_M_header;
    _M_header.next = &_M_header;
  }

  inline file_model::~file_model()
  {
    close();
  }

  inline bool file_model::find(uint64_t off,
                               direction dir,
                               const void* needle,
                               uint64_t needlelen,
                               uint64_t& position) const
  {
    return (dir == direction::kForward) ?
                                          find_forward(off,
                                                       needle,
                                                       needlelen,
                                                       position) :
                                          find_backward(off,
                                                        needle,
                                                        needlelen,
                                                        position);
  }

  inline bool file_model::read_only() const
  {
    return _M_read_only;
  }

  inline bool file_model::block_device() const
  {
    return _M_block_device;
  }

  inline uint64_t file_model::length() const
  {
    return _M_len;
  }

  inline uint64_t file_model::memory_used() const
  {
    return _M_memory_used;
  }

  inline bool file_model::modified() const
  {
    return _M_modified;
  }

  inline bool file_model::seek(uint64_t off,
                               struct block*& b,
                               uint64_t& pos) const
  {
    return seek(off, const_cast<const struct block*&>(b), pos);
  }
}

#endif // FS_FILE_MODEL_H
