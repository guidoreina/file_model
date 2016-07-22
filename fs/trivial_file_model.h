#ifndef FS_TRIVIAL_FILE_MODEL_H
#define FS_TRIVIAL_FILE_MODEL_H

#include <stdint.h>
#include <sys/mman.h>
#include <limits.h>
#include "types/direction.h"

namespace fs {
  class trivial_file_model {
    public:
      // Constructor.
      trivial_file_model();

      // Destructor.
      ~trivial_file_model();

      // Open.
      bool open(const char* filename);

      // Close.
      void close();

      // Modify.
      bool modify(uint64_t off, const void* data, uint64_t len);

      // Add.
      bool add(uint64_t off, const void* data, uint64_t len);

      // Remove.
      bool remove(uint64_t off, uint64_t len);

      // Get data.
      bool get(uint64_t off, void* data, uint64_t& len) const;

      // Find.
      bool find(uint64_t off,
                direction dir,
                const void* needle,
                uint64_t needlelen,
                uint64_t& pos) const;

      // Read only mode?
      bool read_only() const;

      // Is block device?
      bool block_device() const;

      // Get length.
      uint64_t length() const;

    private:
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

      // Write.
      static uint64_t pwrite(int fd,
                             const void* buf,
                             uint64_t len,
                             uint64_t offset);

      // Disable copy constructor and assignment operator.
      trivial_file_model(const trivial_file_model&) = delete;
      trivial_file_model& operator=(const trivial_file_model&) = delete;
  };

  inline trivial_file_model::trivial_file_model()
    : _M_fd(-1),
      _M_read_only(true),
      _M_block_device(false),
      _M_filesize(0),
      _M_data(MAP_FAILED)
  {
    *_M_filename = 0;
  }

  inline trivial_file_model::~trivial_file_model()
  {
    close();
  }

  inline bool trivial_file_model::find(uint64_t off,
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

  inline bool trivial_file_model::read_only() const
  {
    return _M_read_only;
  }

  inline bool trivial_file_model::block_device() const
  {
    return _M_block_device;
  }

  inline uint64_t trivial_file_model::length() const
  {
    return _M_filesize;
  }
}

#endif // FS_TRIVIAL_FILE_MODEL_H
