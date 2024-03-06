#define MODE_READ 0
#define MODE_APPEND 1
#define BLOCKSIZE 2048 // bytes

int vsformat(char *vdiskname, unsigned int m);
int vsmount(char *vdiskname);
int vsumount();
int vscreate(char *filename);
int vsopen(char *filename, int mode);
int vsclose(int fd);
int vssize(int fd);
int vsread(int fd, void *buf, int n);
int vsappend(int fd, void *buf, int n);
int vsdelete(char *filename);
void initializeSuperBlock(int blockCount);
void initializeFatBlocks();
void initializeRootDirectoryBlocks();
void cacheFatTable();
void cacheRootDirectory();
void flushCachedFatTable();
void flushCachedRootDirectory();
void clearOpenFileTable();
void getSuperblock();
void setSuperblock();
void setRootDirectoryEntry(int fd);
int allocateBlockFatEntry(int cacheIndex, int data);
int allocateAndAppendAvailableBlock(int startBlock);
int getLastBlockOfFile(int startBlock);
void deallocateFatEntriesOfFile(int startBlock);
int findAvailableBlockIndex();
int findAvailableDirectoryEntryIndex();
int allocateDirectoryEntry(int cacheIndex, char *filename, int size, int startBlock, int allocationStatus);
int findAvailableOpenFileTableIndex();
int findDirectoryEntryIndexByFilename(char *filename);
void allocateOpenFileTableEntry(int fd, int cacheIndex, int accessMode);
int allocateAvailableBlockForFile(int startBlock);
int readFromBlockToBuffer(char *blockBuffer, int block, int startOffset, int endOffset, int *byteCounter);
int writeFromBufferToBlock(char *blockBuffer, int block, int startOffset, int endOffset, int *byteCounter, int writeSize);
void deallocateDirectoryEntry(int cacheIndex);
