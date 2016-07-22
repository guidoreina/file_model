#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "fs/random_file.h"

bool fs::random_file(const char* filename, uint64_t len)
{
  FILE* file;
  if ((file = fopen(filename, "w+")) == NULL) {
    return false;
  }

  srandom(time(NULL));

  uint64_t i;
  for (i = 0; i + sizeof(int) < len; i += sizeof(int)) {
    int n = random();
    fwrite(&n, sizeof(int), 1, file);
  }

  if (i < len) {
    int n = random();
    fwrite(&n, 1, len - i, file);
  }

  fclose(file);

  return true;
}
