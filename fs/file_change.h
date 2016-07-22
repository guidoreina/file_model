#ifndef FS_FILE_CHANGE_H
#define FS_FILE_CHANGE_H

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

namespace fs {
  struct file_change {
    enum class type {
      kModify,
      kAdd,
      kRemove
    };

    type t;

    uint64_t off;

    uint8_t* olddata;
    uint8_t* newdata;

    uint64_t len;
  };

  class file_changes {
    public:
      // Constructor.
      file_changes();

      // Destructor.
      ~file_changes();

      // Clear.
      void clear();

      // Load.
      bool load(const char* filename);

      // Save.
      bool save(const char* filename) const;

      // Modify.
      bool modify(uint64_t off,
                  void* olddata,
                  const void* newdata,
                  uint64_t len);

      // Add.
      bool add(uint64_t off, const void* newdata, uint64_t len);

      // Remove.
      bool remove(uint64_t off, void* olddata, uint64_t len);

      // Register change.
      bool register_change(const file_change& change);
      bool register_change(file_change::type type,
                           uint64_t off,
                           void* olddata,
                           const void* newdata,
                           uint64_t len);

      // Erase last change.
      bool erase_last_change();

      // Erase from position.
      bool erase_from_position(size_t pos);

      // Get number of changes.
      size_t size() const;

      // Get change.
      const struct file_change* get(size_t pos) const;

    private:
      file_change* _M_changes;
      size_t _M_size;
      size_t _M_used;

      // Allocate.
      bool allocate();

      // Hexadecimal dump.
      static void hexdump(FILE* file, const uint8_t* data, uint64_t len);

      // Disable copy constructor and assignment operator.
      file_changes(const file_changes&) = delete;
      file_changes& operator=(const file_changes&) = delete;
  };

  inline file_changes::file_changes()
    : _M_changes(NULL),
      _M_size(0),
      _M_used(0)
  {
  }

  inline file_changes::~file_changes()
  {
    clear();
  }

  inline bool file_changes::modify(uint64_t off,
                                   void* olddata,
                                   const void* newdata,
                                   uint64_t len)
  {
    return register_change(file_change::type::kModify,
                           off,
                           olddata,
                           newdata,
                           len);
  }

  inline bool file_changes::add(uint64_t off, const void* newdata, uint64_t len)
  {
    return register_change(file_change::type::kAdd, off, NULL, newdata, len);
  }

  inline bool file_changes::remove(uint64_t off,
                                   void* olddata,
                                   uint64_t len)
  {
    return register_change(file_change::type::kRemove, off, olddata, NULL, len);
  }

  inline bool file_changes::register_change(const file_change& change)
  {
    return register_change(change.t,
                           change.off,
                           change.olddata,
                           change.newdata,
                           change.len);
  }

  inline size_t file_changes::size() const
  {
    return _M_used;
  }

  inline const struct file_change* file_changes::get(size_t pos) const
  {
    return (pos < _M_used) ? &_M_changes[pos] : NULL;
  }
}

#endif // FS_FILE_CHANGE_H
