#ifndef BACKUP_H
#define BACKUP_H

#include "types.h"

int backup_create(const char* archive_path);
int backup_restore(const char* archive_path);

#endif
