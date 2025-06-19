#ifndef UTILS_H
#define UTILS_H

char* read_file(const char* filename);

#ifdef __DEBUG
  #define _dprintf fprint
#else
  #define _dprintf(...)
#endif

#endif // UTILS_H
