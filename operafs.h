#ifndef EFMM_OPERAFS_H
#define EFMM_OPERAFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct OperaFS OperaFS;
   typedef struct OperaFile OperaFile;

   int OperaFS_Mount(const char *imagePath);
   void OperaFS_Unmount(void);
   int OperaFS_IsMounted(void);
   const char *OperaFS_GetImagePath(void);

   OperaFile *OperaFS_Open(const char *path);
   size_t OperaFS_Read(OperaFile *file, void *buffer, size_t bytes);
   int64_t OperaFS_Seek(OperaFile *file, int64_t offset, int whence);
   int64_t OperaFS_Tell(OperaFile *file);
   int64_t OperaFS_Size(OperaFile *file);
   void OperaFS_Close(OperaFile *file);

   int OperaFS_ChangeDirectory(const char *path);
   const char *OperaFS_GetCurrentDirectory(void);

   int OperaFS_LoadFile(const char *path, uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
