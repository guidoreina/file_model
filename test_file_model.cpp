#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "fs/file_model.h"
#include "fs/trivial_file_model.h"
#include "fs/file_change.h"
#include "fs/random_file.h"
#include "fs/copy.h"
#include "fs/diff.h"

static const char* kFileModelName = "file_model.bin";
static const char* kOriginalFile = "file_model.org";
static const char* kTrivialFileModelName = "trivial_file_model.bin";
static const size_t kRandomFileMinSize = 100 * 1024;
static const size_t kRandomFileMaxSize = 10 * 1024 * 1024;
static const uint64_t kMinSearch = 4 * 1024;
static const uint64_t kMaxSearch = 32 * 1024;

static bool generate_random_changes(fs::file_changes& changes);
static bool perform_changes(const fs::file_changes& changes,
                            fs::file_model& file_model,
                            fs::trivial_file_model& trivial_file_model);

static bool perform_change(const fs::file_change* change,
                           fs::file_model& file_model,
                           fs::trivial_file_model& trivial_file_model);

static bool perform_searches(const fs::file_model& file_model,
                             const fs::trivial_file_model& trivial_file_model);

static bool perform_search(uint64_t pos,
                           uint64_t needlelen,
                           uint64_t forwardoff,
                           uint64_t backwardoff,
                           const fs::file_model& file_model,
                           const fs::trivial_file_model& trivial_file_model);

static bool perform_search(const uint8_t* needle,
                           uint64_t needlelen,
                           direction dir,
                           uint64_t off,
                           const fs::file_model& file_model,
                           const fs::trivial_file_model& trivial_file_model);

static bool perform_undos(fs::file_model& file_model, size_t nchanges);
static bool perform_redos(fs::file_model& file_model, size_t nchanges);

static bool remove_all(fs::file_model& file_model,
                       fs::trivial_file_model& trivial_file_model);

static bool fill_random_data(fs::file_model& file_model,
                             fs::trivial_file_model& trivial_file_model);

static bool generate_file_models();

static bool equal(const fs::file_model& file_model,
                  const fs::trivial_file_model& trivial_file_model);

static void fill_random_data(uint8_t* data, size_t len);

int main(int argc, const char** argv)
{
  if (argc > 2) {
    fprintf(stderr, "Usage: %s [<changes-file>]\n", argv[0]);
    return -1;
  }

  fs::file_changes changes;

  if (argc == 2) {
    if (!changes.load(argv[1])) {
      fprintf(stderr, "Couldn't load changes from file '%s'.\n", argv[1]);
      return -1;
    }
  } else {
    // Generate random changes.
    if (!generate_random_changes(changes)) {
      return -1;
    }
  }

  // Generate file models.
  if (!generate_file_models()) {
    return -1;
  }

  // Open files.
  fs::file_model file_model;
  if (!file_model.open(kFileModelName)) {
    fprintf(stderr, "Error opening file %s.\n", kFileModelName);
    return -1;
  }

  fs::trivial_file_model trivial_file_model;
  if (!trivial_file_model.open(kTrivialFileModelName)) {
    fprintf(stderr, "Error opening file %s.\n", kTrivialFileModelName);
    return -1;
  }

  // Perform changes.
  if (!perform_changes(changes, file_model, trivial_file_model)) {
    return -1;
  }

  if (argc == 2) {
    if (!file_model.save()) {
      fprintf(stderr, "Error saving file_model.\n");
      return -1;
    }

    if (!fs::diff(kFileModelName, kTrivialFileModelName)) {
      fprintf(stderr,
              "Files %s and %s are different.\n",
              kFileModelName,
              kTrivialFileModelName);

      return -1;
    }

    return 0;
  }

  // Perform searches.
  if (!perform_searches(file_model, trivial_file_model)) {
    return -1;
  }

  // Perform undos.
  if (!perform_undos(file_model, changes.size())) {
    return -1;
  }

  // Perform redos.
  if (!perform_redos(file_model, changes.size())) {
    return -1;
  }

  // Empty files.
  if (!remove_all(file_model, trivial_file_model)) {
    return -1;
  }

  // Fill files with random data.
  if (!fill_random_data(file_model, trivial_file_model)) {
    return -1;
  }

  return 0;
}

bool generate_random_changes(fs::file_changes& changes)
{
  printf("Generating random changes...\n");

  static const unsigned kNumberChanges = 1000;
  static const size_t kMaxChangeSize = 32 * 1024;

  // Generate random file size.
  srandom(time(NULL));
  uint64_t filesize = (random() %
                       (kRandomFileMaxSize - kRandomFileMinSize + 1)) +
                      kRandomFileMinSize;

  // Generate random file.
  if (!fs::random_file(kOriginalFile, filesize)) {
    fprintf(stderr, "Error generating random file of %llu bytes.\n", filesize);
    return false;
  }

  fs::file_change change;
  change.olddata = NULL;

  // Generate random changes.
  do {
    uint64_t len = random() % (kMaxChangeSize + 1);

    uint64_t off;
    if (filesize > 0) {
      off = random() % filesize;
    } else {
      off = 0;
    }

    // Switch random change.
    uint8_t buf[kMaxChangeSize];
    switch (random() % 3) {
      case 0: // Modify.
        // If the file is empty...
        if (filesize == 0) {
          continue;
        }

        if (off + len > filesize) {
          len = filesize - off;
        }

        fill_random_data(buf, len);

        change.t = fs::file_change::type::kModify;
        change.newdata = buf;

        break;
      case 1: // Add.
        fill_random_data(buf, len);

        change.t = fs::file_change::type::kAdd;
        change.newdata = buf;

        filesize += len;

        break;
      default: // Remove.
        // If the file is empty...
        if (filesize == 0) {
          continue;
        }

        if (off + len > filesize) {
          len = filesize - off;
        }

        change.t = fs::file_change::type::kRemove;
        change.newdata = NULL;

        filesize -= len;
    }

    change.off = off;
    change.len = len;

    // Record change.
    if (!changes.register_change(change)) {
      fprintf(stderr, "Error recording change.\n");
      return false;
    }
  } while (changes.size() < kNumberChanges);

  // If the file is not empty...
  if (filesize != 0) {
    // Modify the whole file.
    uint8_t* data;
    if ((data = reinterpret_cast<uint8_t*>(malloc(filesize))) == NULL) {
      fprintf(stderr, "Cannot allocate %llu bytes of memory.\n", filesize);
      return false;
    }

    fill_random_data(data, filesize);

    change.t = fs::file_change::type::kModify;
    change.off = 0;
    change.newdata = data;
    change.len = filesize;

    // Record change.
    if (!changes.register_change(change)) {
      fprintf(stderr, "Error recording change.\n");

      free(data);
      return false;
    }

    free(data);
  }

  // Save changes.
  if (!changes.save("changes.txt")) {
    fprintf(stderr, "Error saving changes.\n");
    return false;
  }

  return true;
}

bool perform_changes(const fs::file_changes& changes,
                     fs::file_model& file_model,
                     fs::trivial_file_model& trivial_file_model)
{
  printf("Performing changes...\n");
  for (size_t i = 0; i < changes.size(); i++) {
    // Perform change and compare files.
    if ((!perform_change(changes.get(i), file_model, trivial_file_model)) ||
        (!equal(file_model, trivial_file_model))) {
      return false;
    }
  }

  return true;
}

bool perform_change(const fs::file_change* change,
                    fs::file_model& file_model,
                    fs::trivial_file_model& trivial_file_model)
{
  fs::file_model::operation_result res;

  switch (change->t) {
    case fs::file_change::type::kModify:
      if (!trivial_file_model.modify(change->off,
                                     change->newdata,
                                     change->len)) {
        fprintf(stderr,
                "Error modifying trivial_file_model (offset = %llu, "
                "length = %llu).\n",
                change->off,
                change->len);

        return false;
      }

      if ((res = file_model.modify(change->off,
                                   change->newdata,
                                   change->len)) !=
          fs::file_model::operation_result::kSuccess) {
        fprintf(stderr,
                "[Modify] [Offset = %llu, length = %llu] %s\n",
                change->off,
                change->len,
                fs::file_model::operation_result_to_string(res));

        return false;
      }

      break;
    case fs::file_change::type::kAdd:
      if (!trivial_file_model.add(change->off, change->newdata, change->len)) {
        fprintf(stderr,
                "Error adding to trivial_file_model (offset = %llu, "
                "length = %llu).\n",
                change->off,
                change->len);

        return false;
      }

      if ((res = file_model.add(change->off, change->newdata, change->len)) !=
          fs::file_model::operation_result::kSuccess) {
        fprintf(stderr,
                "[Add] [Offset = %llu, length = %llu] %s\n",
                change->off,
                change->len,
                fs::file_model::operation_result_to_string(res));

        return false;
      }

      break;
    case fs::file_change::type::kRemove:
      if (!trivial_file_model.remove(change->off, change->len)) {
        fprintf(stderr,
                "Error removing from trivial_file_model (offset = %llu, "
                "length = %llu).\n",
                change->off,
                change->len);

        return false;
      }

      if ((res = file_model.remove(change->off, change->len)) !=
          fs::file_model::operation_result::kSuccess) {
        fprintf(stderr,
                "[Remove] [Offset = %llu, length = %llu] %s\n",
                change->off,
                change->len,
                fs::file_model::operation_result_to_string(res));

        return false;
      }

      break;
  }

  return true;
}

bool perform_searches(const fs::file_model& file_model,
                      const fs::trivial_file_model& trivial_file_model)
{
  static const unsigned kNumberSearches = 1000;

  // If the file is empty...
  if (trivial_file_model.length() == 0) {
    printf("File is empty => no search.\n");
    return true;
  }

  // Search.
  printf("Searching...\n");

  for (unsigned i = 0; i < kNumberSearches; i++) {
    uint64_t len = (random() % (kMaxSearch - kMinSearch + 1)) + kMinSearch;
    uint64_t pos = random() % trivial_file_model.length();

    if (pos + len > trivial_file_model.length()) {
      len = trivial_file_model.length() - pos;
    }

    if (!perform_search(pos,
                        len,
                        random() % (pos + 1),
                        (random() % (trivial_file_model.length() - pos)) + pos,
                        file_model,
                        trivial_file_model)) {
      return false;
    }
  }

  // Search at the beginning.
  uint64_t len = (kMaxSearch < trivial_file_model.length()) ?
                                                    kMaxSearch :
                                                    trivial_file_model.length();

  if (!perform_search(0,
                      len,
                      0,
                      trivial_file_model.length() - 1,
                      file_model,
                      trivial_file_model)) {
    return false;
  }

  // Search at the end.
  if (!perform_search(trivial_file_model.length() - len,
                      len,
                      0,
                      trivial_file_model.length() - 1,
                      file_model,
                      trivial_file_model)) {
    return false;
  }

  return true;
}

bool perform_search(uint64_t pos,
                    uint64_t needlelen,
                    uint64_t forwardoff,
                    uint64_t backwardoff,
                    const fs::file_model& file_model,
                    const fs::trivial_file_model& trivial_file_model)
{
  // Get the data to be searched from the trivial_file_model.
  uint8_t needle[kMaxSearch];
  uint64_t len = needlelen;
  if ((!trivial_file_model.get(pos, needle, len)) ||
      (len != needlelen)) {
    fprintf(stderr,
            "Error getting data from the trivial_file_model (offset: %llu, "
            "length: %llu, file size: %llu).\n",
            pos,
            needlelen,
            trivial_file_model.length());

    return false;
  }

  // Search forward.
  if (!perform_search(needle,
                      needlelen,
                      direction::kForward,
                      forwardoff,
                      file_model,
                      trivial_file_model)) {
    return false;
  }

  // Search backward.
  if (!perform_search(needle,
                      needlelen,
                      direction::kBackward,
                      backwardoff,
                      file_model,
                      trivial_file_model)) {
    return false;
  }

  return true;
}

bool perform_search(const uint8_t* needle,
                    uint64_t needlelen,
                    direction dir,
                    uint64_t off,
                    const fs::file_model& file_model,
                    const fs::trivial_file_model& trivial_file_model)
{
  uint64_t pos2;
  if (!trivial_file_model.find(off,
                               dir,
                               needle,
                               needlelen,
                               pos2)) {
    fprintf(stderr,
            "[%s] Needle not found in trivial_file_model "
            "(offset: %llu, length: %llu).\n",
            (dir == direction::kForward) ? "Forward" : "Backward",
            off,
            needlelen);

    return false;
  }

  uint64_t pos1;
  if (!file_model.find(off,
                       dir,
                       needle,
                       needlelen,
                       pos1)) {
    fprintf(stderr,
            "[%s] Needle not found in file_model (offset: %llu, "
            "length: %llu).\n",
            (dir == direction::kForward) ? "Forward" : "Backward",
            off,
            needlelen);

    return false;
  }

  if (pos1 != pos2) {
    fprintf(stderr,
            "[%s] Positions are different (file_model: %llu, "
            "trivial_file_model: %llu, offset: %llu, length: %llu).\n",
            (dir == direction::kForward) ? "Forward" : "Backward",
            pos1,
            pos2,
            off,
            needlelen);

    return false;
  }

  return true;
}

bool perform_undos(fs::file_model& file_model, size_t nchanges)
{
  printf("Performing undos...\n");

  for (size_t i = 0; i < nchanges; i++) {
    fs::file_model::operation_result res;
    if ((res = file_model.undo()) !=
        fs::file_model::operation_result::kSuccess) {
      fprintf(stderr,
              "[Undo] %s\n",
              fs::file_model::operation_result_to_string(res));

      return false;
    }
  }

  if (!file_model.save()) {
    fprintf(stderr, "Error saving file_model.\n");
    return false;
  }

  if (!fs::diff(kOriginalFile, kFileModelName)) {
    fprintf(stderr,
            "Files %s and %s are different.\n",
            kOriginalFile,
            kFileModelName);

    return false;
  }

  return true;
}

bool perform_redos(fs::file_model& file_model, size_t nchanges)
{
  printf("Performing redos...\n");

  for (size_t i = 0; i < nchanges; i++) {
    fs::file_model::operation_result res;
    if ((res = file_model.redo()) !=
        fs::file_model::operation_result::kSuccess) {
      fprintf(stderr,
              "[Redo] %s\n",
              fs::file_model::operation_result_to_string(res));

      return false;
    }
  }

  if (!file_model.save()) {
    fprintf(stderr, "Error saving file_model.\n");
    return false;
  }

  if (!fs::diff(kFileModelName, kTrivialFileModelName)) {
    fprintf(stderr,
            "Files %s and %s are different.\n",
            kFileModelName,
            kTrivialFileModelName);

    return false;
  }

  return true;
}

bool remove_all(fs::file_model& file_model,
                fs::trivial_file_model& trivial_file_model)
{
  printf("Removing all...\n");

  if (!trivial_file_model.remove(0, trivial_file_model.length())) {
    fprintf(stderr, "Error emptying trivial_file_model.\n");
    return false;
  }

  fs::file_model::operation_result res;
  if ((res = file_model.remove(0, file_model.length())) !=
      fs::file_model::operation_result::kSuccess) {
    fprintf(stderr,
            "Error emptying file_model (%s).\n",
            fs::file_model::operation_result_to_string(res));

    return false;
  }

  if (!file_model.save()) {
    fprintf(stderr, "Error saving file_model.\n");
    return false;
  }

  if (!fs::diff(kFileModelName, kTrivialFileModelName)) {
    fprintf(stderr,
            "The files %s and %s are different.\n",
            kFileModelName,
            kTrivialFileModelName);

    return false;
  }

  return true;
}

bool fill_random_data(fs::file_model& file_model,
                      fs::trivial_file_model& trivial_file_model)
{
  printf("Filling with random data...\n");

  // Generate random file size.
  uint64_t filesize = (random() %
                       (kRandomFileMaxSize - kRandomFileMinSize + 1)) +
                      kRandomFileMinSize;

  uint8_t* data;
  if ((data = reinterpret_cast<uint8_t*>(malloc(filesize))) == NULL) {
    fprintf(stderr, "Error allocating %llu bytes of memory.\n", filesize);
    return false;
  }

  fill_random_data(data, filesize);

  if (!trivial_file_model.add(0, data, filesize)) {
    fprintf(stderr,
            "Error adding %llu bytes to trivial_file_model.\n",
            filesize);

    free(data);
    return false;
  }

  fs::file_model::operation_result res;
  if ((res = file_model.add(0, data, filesize)) !=
      fs::file_model::operation_result::kSuccess) {
    fprintf(stderr,
            "Error adding %llu bytes to file_model (%s).\n",
            filesize,
            fs::file_model::operation_result_to_string(res));

    free(data);
    return false;
  }

  free(data);

  if (!file_model.save()) {
    fprintf(stderr, "Error saving file_model.\n");
    return false;
  }

  if (!fs::diff(kFileModelName, kTrivialFileModelName)) {
    fprintf(stderr,
            "The files %s and %s are different.\n",
            kFileModelName,
            kTrivialFileModelName);

    return false;
  }

  return true;
}

bool generate_file_models()
{
  static const char* copies[] = {kFileModelName, kTrivialFileModelName};

  for (size_t i = 0; i < 2; i++) {
    // Generate file.
    if (!fs::copy(kOriginalFile, copies[i])) {
      fprintf(stderr,
              "Error copying file \"%s\" -> \"%s\".\n",
              kOriginalFile,
              copies[i]);

      return false;
    }

    // Check that the files are the same.
    if (!fs::diff(kOriginalFile, copies[i])) {
      fprintf(stderr,
              "The files \"%s\" and \"%s\" are different.\n",
              kOriginalFile,
              copies[i]);

      return false;
    }
  }

  return true;
}

bool equal(const fs::file_model& file_model,
           const fs::trivial_file_model& trivial_file_model)
{
  static const size_t kReadBufferSize = 4 * 1024;

  if (file_model.length() != trivial_file_model.length()) {
    fprintf(stderr,
            "Files are different (file_model: %llu, trivial_file_model: "
            "%llu).\n",
            file_model.length(),
            trivial_file_model.length());

    return false;
  }

  uint8_t buf1[kReadBufferSize], buf2[kReadBufferSize];
  uint64_t len1, len2;
  uint64_t off1, off2;

  len1 = sizeof(buf1);
  len2 = sizeof(buf2);

  off1 = 0;
  off2 = 0;

  while ((file_model.get(off1, buf1, len1)) &&
         (trivial_file_model.get(off2, buf2, len2))) {
    if (len1 != len2) {
      fprintf(stderr,
              "Lengths are different (file_model: %llu, trivial_file_model: "
              "%llu).\n",
              len1,
              len2);

      return false;
    }

    if (memcmp(buf1, buf2, len1) != 0) {
      fprintf(stderr, "Contents are different.\n");
      return false;
    }

    off1 += len1;
    off2 += len2;

    len1 = sizeof(buf1);
    len2 = sizeof(buf2);
  }

  if (file_model.get(off1, buf1, len1)) {
    fprintf(stderr, "Could read from file_model.\n");
    return false;
  }

  if (trivial_file_model.get(off1, buf1, len1)) {
    fprintf(stderr, "Could read from trivial_file_model.\n");
    return false;
  }

  return true;
}

void fill_random_data(uint8_t* data, size_t len)
{
  size_t i;
  for (i = 0; i + sizeof(int) < len; i += sizeof(int)) {
    int n = random();
    memcpy(data, &n, sizeof(int));
    data += sizeof(int);
  }

  if (i < len) {
    int n = random();
    memcpy(data, &n, len - i);
  }
}
