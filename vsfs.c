#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "vsfs.h"

#define SUPERBLOCK_START 0 // Block 0
#define SUPERBLOCK_COUNT 1
#define FAT_BLOCK_START 1 // Blocks 1, ..., 32
#define FAT_BLOCK_COUNT 32
#define ROOT_DIR_START 33 // Blocks 33, ..., 40
#define ROOT_DIR_COUNT 8
#define METADATA_BLOCK_SIZE (SUPERBLOCK_COUNT + FAT_BLOCK_COUNT + ROOT_DIR_COUNT)
#define FAT_ENTRY_SIZE 4                                        // Bytes
#define FAT_ENTRY_PER_BLOCK (BLOCKSIZE / FAT_ENTRY_SIZE)        // 512 entries
#define FAT_ENTRY_COUNT (FAT_ENTRY_PER_BLOCK * FAT_BLOCK_COUNT) // Bytes
#define MAX_DISK_SIZE_SHIFT 23                                  // Shift amount
#define MIN_DISK_SIZE_SHIFT 18                                  // Shift amount
#define DIR_ENTRY_SIZE 128                                      // Bytes
#define DIR_ENTRY_PER_BLOCK (BLOCKSIZE / DIR_ENTRY_SIZE)        // 4KB (BLOCKSIZE)/128 Bytes (DIR_ENTRY_SIZE)
#define DIR_ENTRY_COUNT (DIR_ENTRY_PER_BLOCK * ROOT_DIR_COUNT)  // 4(ROOT_DIR_COUNT) * 32 (DIR_ENTRY_PER_BLOCK)
#define MAX_FILENAME_LENGTH 30
#define MAX_NOF_OPEN_FILES 16
#define NOT_USED_FLAG 0
#define USED_FLAG 1
#define EOF_FLAG -1

struct dirEntry
{
    char filename[MAX_FILENAME_LENGTH]; // 30 Bytes
    int size;                           // 4 Bytes
    int startBlock;                     // 4 Bytes
    int allocated;                      // 4 Bytes
};

struct fatEntry
{
    int nextBlockIndex; // 4 bytes
};

struct fileStruct
{
    int accessMode;         // 4 Bytes
    int dirBlock;           // 4 Bytes
    int dirBlockOffset;     // 4 Bytes
    int cachedRootDirIndex; // 4 Bytes
    int positionPtr;        // 4 Bytes
};

// Globals =======================================
int vs_fd; // File descriptor of the Linux file.
// The Linux file is our disk.
// This descriptor is not visible to an application.
// ========================================================

// Initialized by the superblock
int dataBlockCount;
int totalBlockCount;
int freeBlockCount;
int fileCount;

int openFileCount = 0;
struct fileStruct openFileTable[MAX_NOF_OPEN_FILES];
struct fatEntry cachedFatTable[FAT_ENTRY_COUNT];
struct dirEntry cachedRootDirectory[DIR_ENTRY_COUNT];

int read_block(void *block, int k)
{
    int n;
    int offset;
    offset = k * BLOCKSIZE;
    lseek(vs_fd, (off_t)offset, SEEK_SET);
    n = read(vs_fd, block, BLOCKSIZE);
    if (n != BLOCKSIZE)
    {
        printf("read error\n");
        return -1;
    }
    return (0);
}

int write_block(void *block, int k)
{
    int n;
    int offset;
    offset = k * BLOCKSIZE;
    lseek(vs_fd, (off_t)offset, SEEK_SET);
    n = write(vs_fd, block, BLOCKSIZE);
    if (n != BLOCKSIZE)
    {
        printf("write error\n");
        return (-1);
    }
    return 0;
}

int vsformat(char *vdiskname, unsigned int m)
{
    // Meta information operations
    char command[1000];
    int size;
    int num = 1;
    int count;
    size = num << m;
    count = size / BLOCKSIZE;
    sprintf(command, "dd if=/dev/zero of=%s bs=%d count=%d",
            vdiskname, BLOCKSIZE, count);
    system(command);

    // Open file decriptor
    vs_fd = open(vdiskname, O_RDWR);

    // Initialize Super blocks on virtual disk
    initializeSuperBlock(count);
    // Initialize FAT entry blocks on virtual disk
    initializeFatBlocks(count);
    // Initialize Root Directory entry blocks on virtual disk
    initializeRootDirectoryBlocks();

    // Synchronize memory & disk then close descriptor
    fsync(vs_fd);
    close(vs_fd);
    return (0);
}

int vsmount(char *vdiskname)
{
    // Open file descriptor "globally"
    vs_fd = open(vdiskname, O_RDWR);

    // Read super block information on virtual disk to memory
    getSuperblock();
    // Read FAT entries on virtual disk to memory cache
    cacheFatTable();
    // Read Root Directory entries on virtual disk to memory cache
    cacheRootDirectory();

    // Clear (initialize) the system wide open file table
    clearOpenFileTable();
    return (0);
}

int vsumount()
{
    // Write super block information on memory to virtual disk
    setSuperblock();
    // Write FAT entries on memory cache to virtual disk
    flushCachedFatTable();
    // Write Root Directory entries on memory cache to virtual disk
    flushCachedRootDirectory();

    // Close all file descriptors on Open File Table
    for (int i = 0; i < MAX_NOF_OPEN_FILES; i++)
    {
        if (openFileTable[i].dirBlock > -1)
        {
            vsclose(i);
        }
    }

    // Synchronize memory & disk then close descriptor
    fsync(vs_fd);
    close(vs_fd);
    return (0);
}

int vscreate(char *filename)
{
    // Check number of available files on root directory
    if (fileCount == DIR_ENTRY_COUNT)
    {
        printf("ERROR: No capacity available for file creation!\n");
        return -1;
    }

    // Check same named file existence
    for (int i = 0; i < DIR_ENTRY_COUNT; i++)
    {
        if (cachedRootDirectory[i].allocated == USED_FLAG && strcmp(cachedRootDirectory[i].filename, filename) == 0)
        {
            printf("ERROR: File with the same name already created!\n");
            return -1;
        }
    }

    // Check for empty data block
    int blockIndex = findAvailableBlockIndex();
    if (blockIndex == -1)
    {
        printf("ERROR: No empty data blocks, can not create a new file!\n");
        return -1;
    }

    // Find empty Directory Root poisiton on cachedRootDirectory
    int availableDirectoryEntryIndex = findAvailableDirectoryEntryIndex();

    // Check non-empty root directory anomaly (Trivial)
    if (availableDirectoryEntryIndex == -1)
    {
        printf("ERROR: No empty root directory was found (Anomaly)!\n");
        return -1;
    }

    // Allocate a new data block for file
    allocateBlockFatEntry(blockIndex, EOF_FLAG);

    // Decrement free block count
    freeBlockCount--;

    // Allocate a new directory entry
    allocateDirectoryEntry(availableDirectoryEntryIndex, filename, 0, blockIndex, USED_FLAG);
    // Increment the number of files
    fileCount++;

    return (0);
}

int vsopen(char *file, int mode)
{
    // Check limit of opening files
    if (openFileCount == MAX_NOF_OPEN_FILES)
    {
        printf("ERROR: Can't open more files!\n");
        return -1;
    }

    // Check maximum number of files opened
    for (int i = 0; i < MAX_NOF_OPEN_FILES; i++)
    {
        char *filename = cachedRootDirectory[openFileTable[i].cachedRootDirIndex].filename;
        if (openFileTable[i].dirBlock > -1 && strcmp(filename, file) == 0)
        {
            printf("ERROR: File already opened!\n");
            return -1;
        }
    }

    // Find space in the open file table
    int fd = findAvailableOpenFileTableIndex();

    if (fd == -1)
    {
        printf("ERROR: Could not find available open file table entry (Anomaly)!\n");
    }

    // Find in the root directory structure by filename
    int directoryEntryIndex = findDirectoryEntryIndexByFilename(file);

    if (directoryEntryIndex == -1)
    {
        printf("ERROR: Could not find the file with the given name!\n");
    }

    // Allocate open file table entry
    allocateOpenFileTableEntry(fd, directoryEntryIndex, mode);

    return fd;
}

int vsclose(int fd)
{
    // Check if file is opened
    if (openFileTable[fd].dirBlock == -1)
    {
        printf("ERROR: File not opened yet\n");
        return -1;
    }

    // Make related open file table available
    openFileTable[fd].dirBlock = -1;
    // Decrement open file count
    openFileCount--;
    return (0);
}

int vssize(int fd)
{
    if (openFileTable[fd].dirBlock == -1)
    {
        printf("ERROR: File not opened yet!\n");
        return -1;
    }

    int size = cachedRootDirectory[openFileTable[fd].cachedRootDirIndex].size;
    if (size < 0)
    {
        printf("ERROR: The file does not have a valid size!\n");
        return -1;
    }

    return size;
}

int vsread(int fd, void *buf, int n)
{
    if (openFileTable[fd].dirBlock == -1)
    {
        printf("ERROR: File not opened yet!\n");
        return -1;
    }

    // Check the correct mode
    if (openFileTable[fd].accessMode == MODE_APPEND)
    {
        printf("ERROR: can't read in APPEND mode!\n");
        return -1;
    }

    // get the data about the directory entry
    struct dirEntry *tmpDirEntry = &(cachedRootDirectory[openFileTable[fd].cachedRootDirIndex]);
    int logicalStartOffset = openFileTable[fd].positionPtr;
    int logicalEndOffset = logicalStartOffset + n;

    if (tmpDirEntry->size < logicalEndOffset)
    {
        printf("ERROR: Cannot reads n bytes exceeding file size!\n");
        return -1;
    }

    // find the block range for acessing data in the file
    int logicalStartBlock = logicalStartOffset / BLOCKSIZE;
    int logicalStartBlockOffset = logicalStartOffset % BLOCKSIZE;

    int logicalEndBlock = logicalEndOffset / BLOCKSIZE;
    int logicalEndBlockOffset = logicalEndOffset % BLOCKSIZE;

    int byteCount = 0;
    void *bufferPtr = buf;

    // Single Block Read
    if (logicalStartBlock == logicalEndBlock)
    {
        readFromBlockToBuffer(bufferPtr, tmpDirEntry->startBlock, logicalStartBlockOffset, logicalEndBlockOffset, &byteCount);
    }
    else
    {
        int blockPtr = tmpDirEntry->startBlock;
        for (int i = logicalStartBlock; i < logicalEndBlock; i++)
        {
            if (blockPtr == -1)
            {
                printf("ERROR(CRITICAL): can't fetch the block in range to read/ not allocated yet!\n");
                return -1;
            }

            // Start partial block read
            if (i == logicalStartBlock)
            {
                readFromBlockToBuffer(bufferPtr, blockPtr, logicalStartBlockOffset, BLOCKSIZE, &byteCount);
            }
            // End partial block read
            else if (i == logicalEndBlock)
            {
                readFromBlockToBuffer(bufferPtr, blockPtr, 0, logicalEndBlockOffset, &byteCount);
            }
            // Mid full block read
            else
            {
                readFromBlockToBuffer(bufferPtr, blockPtr, 0, BLOCKSIZE, &byteCount);
            }

            blockPtr = cachedFatTable[blockPtr].nextBlockIndex;
        }
    }

    // Increment the file pointer
    openFileTable[fd].positionPtr = logicalEndOffset;

    if (byteCount != n)
    {
        printf("ERROR: Could not read n bytes!\n");
        return -1;
    }

    return byteCount;
}

int vsappend(int fd, void *buf, int n)
{
    // Check correctness of n
    if (n <= 0)
    {
        printf("ERROR: n can't take a negative value! %d\n", n);
        return -1;
    }

    // Check if file is opened
    if (openFileTable[fd].dirBlock == -1)
    {
        printf("ERROR: file must opened first!\n");
        return -1;
    }

    // Check the correct mode
    if (openFileTable[fd].accessMode == MODE_READ)
    {
        printf("ERROR: can't append in READ mode!\n");
        return -1;
    }

    // Get cached directory entry of the file
    struct dirEntry *tmpDirEntry = &(cachedRootDirectory[openFileTable[fd].cachedRootDirIndex]);
    int size = tmpDirEntry->size;

    // Logical block offset of file for last block
    int dataBlockOffset = size % BLOCKSIZE;
    if (size > 0 && size % BLOCKSIZE == 0)
    {
        dataBlockOffset = BLOCKSIZE;
    }

    // Calculate remaining bytes of the last block and required block count for the remaining bytes
    int remainingByte = BLOCKSIZE - dataBlockOffset;
    int requiredBlockCount = ((n - remainingByte) / BLOCKSIZE) + 1;

    // Check if the available bytes in the last block is sufficient
    if (n <= remainingByte)
    {
        requiredBlockCount = 0;
    }

    // Check if enough available blocks exist on the virtual disk
    if (requiredBlockCount > freeBlockCount)
    {
        printf("ERROR: Not enough free blocks available! required: %d  free:%d\n", requiredBlockCount, freeBlockCount);
        return -1;
    }

    int byteCount = 0;
    while (byteCount != n)
    {
        // Full block write
        if (dataBlockOffset == BLOCKSIZE)
        {
            // Allocate a new block
            int newAllocatedBlock = allocateAvailableBlockForFile(tmpDirEntry->startBlock);

            if (newAllocatedBlock == -1)
            {
                printf("ERROR: New block allocation issue!\n");
                return -1;
            }

            writeFromBufferToBlock((char *)buf, newAllocatedBlock, 0, BLOCKSIZE, &byteCount, n);
        }
        // Partial block write
        else
        {
            // Get last block of the file
            int lastBlockOfFile = getLastBlockOfFile(tmpDirEntry->startBlock);
            writeFromBufferToBlock((char *)buf, lastBlockOfFile, dataBlockOffset, BLOCKSIZE, &byteCount, n);

            // Continue with full block writes
            dataBlockOffset = BLOCKSIZE;
        }
    }

    // Modify file size at directory entry both on memory cache and virtual disk
    allocateDirectoryEntry(openFileTable[fd].cachedRootDirIndex, tmpDirEntry->filename, tmpDirEntry->size + n, tmpDirEntry->startBlock, tmpDirEntry->allocated);

    if (byteCount != n)
    {
        printf("ERROR: Could not write n bytes!\n");
        return -1;
    }

    return byteCount;
}

int vsdelete(char *filename)
{
    // Close file descriptor
    for (int i = 0; i < MAX_NOF_OPEN_FILES; i++)
    {
        if (openFileTable[i].dirBlock > -1 && strcmp(filename, cachedRootDirectory[openFileTable[i].cachedRootDirIndex].filename) == 0)
        {
            vsclose(i);
        }
    }

    // Find and deallocate root directory of file on disk
    int directoryIndex = findDirectoryEntryIndexByFilename(filename);
    deallocateDirectoryEntry(directoryIndex);

    // Check if file has a valid startBlock
    if (cachedRootDirectory[directoryIndex].startBlock == -1)
    {
        printf("ERROR: Can't find any directory entry associated with the file\n");
        return -1;
    }

    // Deallocate all FAT entries of the file
    deallocateFatEntriesOfFile(cachedRootDirectory[directoryIndex].startBlock);

    // Decrease file count
    fileCount--;
    return (0);
}

// Virtual Disk & Cache Functions

void getSuperblock()
{
    char block[BLOCKSIZE];
    read_block((void *)block, SUPERBLOCK_START);
    dataBlockCount = ((int *)(block))[0];
    totalBlockCount = ((int *)(block + 4))[0];
    freeBlockCount = ((int *)(block + 8))[0];
    fileCount = ((int *)(block + 12))[0];
}

void setSuperblock()
{
    char block[BLOCKSIZE];
    read_block((void *)block, SUPERBLOCK_START);
    ((int *)(block + 8))[0] = freeBlockCount;
    ((int *)(block + 12))[0] = fileCount;
    write_block((void *)block, SUPERBLOCK_START);
}

void initializeSuperBlock(int blockCount)
{
    char block[BLOCKSIZE];
    ((int *)(block))[0] = blockCount - METADATA_BLOCK_SIZE;     // total data block count
    ((int *)(block + 4))[0] = blockCount;                       // total block count
    ((int *)(block + 8))[0] = blockCount - METADATA_BLOCK_SIZE; // free blocks
    ((int *)(block + 12))[0] = 0;                               // number of files
    write_block((void *)block, SUPERBLOCK_START);
}

void initializeFatBlocks(int totalBlockCount)
{
    char block[BLOCKSIZE];
    // printf("LOG(initializeFatBlocks): %d\n", totalBlockCount);
    for (int i = 0; i < FAT_BLOCK_COUNT; i++)
    {
        for (int j = 0; j < FAT_ENTRY_PER_BLOCK; j++)
        {
            if (i * FAT_ENTRY_PER_BLOCK + j < totalBlockCount)
            {
                ((int *)(block + j * FAT_ENTRY_SIZE))[0] = NOT_USED_FLAG;
            }
            else
            {
                // Mark unavailable block numbers inaccessible
                ((int *)(block + j * FAT_ENTRY_SIZE))[0] = EOF_FLAG;
            }
        }

        write_block((void *)block, FAT_BLOCK_START + i);
    }

    // Mark meta data blocks as allocated
    read_block((void *)block, FAT_BLOCK_START);
    for (int i = 0; i < METADATA_BLOCK_SIZE; i++)
    {
        ((int *)(block + i * FAT_ENTRY_SIZE))[0] = EOF_FLAG;
    }
    write_block((void *)block, FAT_BLOCK_START);
}

void initializeRootDirectoryBlocks()
{
    char block[BLOCKSIZE];
    int allocationStatusOffset = (MAX_FILENAME_LENGTH + 8);
    for (int i = 0; i < DIR_ENTRY_PER_BLOCK; i++)
    {
        // Mark root directories not used
        ((int *)(block + (i * DIR_ENTRY_SIZE) + allocationStatusOffset))[0] = NOT_USED_FLAG;
    }

    // Write to the disk the root directory information
    for (int i = 0; i < ROOT_DIR_COUNT; i++)
    {
        write_block((void *)block, ROOT_DIR_START + i);
    }
}

void cacheFatTable()
{
    char block[BLOCKSIZE];

    for (int i = 0; i < FAT_BLOCK_COUNT; i++)
    {
        read_block((void *)block, FAT_BLOCK_START + i);

        for (int j = 0; j < FAT_ENTRY_PER_BLOCK; j++)
        {
            struct fatEntry *tmpFatEntry = &(cachedFatTable[i * FAT_ENTRY_PER_BLOCK + j]);
            int entryStartOffsetBytes = j * FAT_ENTRY_SIZE;
            tmpFatEntry->nextBlockIndex = ((int *)(block + entryStartOffsetBytes))[0];
        }
    }
}

void cacheRootDirectory()
{
    char block[BLOCKSIZE];

    for (int i = 0; i < ROOT_DIR_COUNT; i++)
    {
        read_block((void *)block, ROOT_DIR_START + i);
        for (int j = 0; j < DIR_ENTRY_PER_BLOCK; j++)
        {
            struct dirEntry *tmpDirEntry = &(cachedRootDirectory[i * DIR_ENTRY_PER_BLOCK + j]);
            int entryStartOffset = j * DIR_ENTRY_SIZE;
            memcpy(tmpDirEntry->filename, (char *)(block + entryStartOffset), MAX_FILENAME_LENGTH);
            tmpDirEntry->size = ((int *)(block + entryStartOffset + MAX_FILENAME_LENGTH))[0];
            tmpDirEntry->startBlock = ((int *)(block + entryStartOffset + MAX_FILENAME_LENGTH + 4))[0];
            tmpDirEntry->allocated = ((int *)(block + entryStartOffset + MAX_FILENAME_LENGTH + 8))[0];
        }
    }
}

void flushCachedFatTable()
{
    char block[BLOCKSIZE];

    for (int i = 0; i < FAT_BLOCK_COUNT; i++)
    {
        for (int j = 0; j < FAT_ENTRY_PER_BLOCK; j++)
        {
            struct fatEntry *tmpFatEntry = &(cachedFatTable[i * FAT_ENTRY_PER_BLOCK + j]);
            int entryStartOffset = j * FAT_ENTRY_SIZE;
            ((int *)(block + entryStartOffset))[0] = tmpFatEntry->nextBlockIndex;
        }
        write_block((void *)block, FAT_BLOCK_START + i);
    }
}

void flushCachedRootDirectory()
{
    char block[BLOCKSIZE];

    for (int i = 0; i < ROOT_DIR_COUNT; i++)
    {
        for (int j = 0; j < DIR_ENTRY_PER_BLOCK; j++)
        {
            struct dirEntry *tmpDirEntry = &(cachedRootDirectory[i * DIR_ENTRY_PER_BLOCK + j]);
            int entryStartOffset = j * DIR_ENTRY_SIZE;
            memcpy((char *)(block + entryStartOffset), tmpDirEntry->filename, MAX_FILENAME_LENGTH);
            ((int *)(block + entryStartOffset + MAX_FILENAME_LENGTH))[0] = tmpDirEntry->size;
            ((int *)(block + entryStartOffset + MAX_FILENAME_LENGTH + 4))[0] = tmpDirEntry->startBlock;
            ((int *)(block + entryStartOffset + MAX_FILENAME_LENGTH + 8))[0] = tmpDirEntry->allocated;
        }
        write_block((void *)block, ROOT_DIR_START + i);
    }
}

void clearOpenFileTable()
{
    for (int i = 0; i < MAX_NOF_OPEN_FILES; i++)
    {
        openFileTable[i].dirBlock = -1;
        openFileCount = 0;
    }
}

int findAvailableDirectoryEntryIndex()
{
    int cacheIndex;
    for (int i = 0; i < ROOT_DIR_COUNT; i++)
    {
        for (int j = 0; j < DIR_ENTRY_PER_BLOCK; j++)
        {
            cacheIndex = i * DIR_ENTRY_PER_BLOCK + j;
            if (cachedRootDirectory[cacheIndex].allocated == NOT_USED_FLAG)
            {
                return cacheIndex;
            }
        }
    }

    // If not empty entry found
    return -1;
}

int findDirectoryEntryIndexByFilename(char *filename)
{
    for (int i = 0; i < DIR_ENTRY_COUNT; i++)
    {
        if (cachedRootDirectory[i].allocated == USED_FLAG && strcmp(filename, cachedRootDirectory[i].filename) == 0)
        {
            return i;
        }
    }

    // Could not be found
    return -1;
}

int findAvailableBlockIndex()
{
    int cacheIndex;
    for (int i = 0; i < FAT_BLOCK_COUNT; i++)
    {
        for (int j = 0; j < FAT_ENTRY_PER_BLOCK; j++)
        {
            cacheIndex = i * FAT_ENTRY_PER_BLOCK + j;
            if (cachedFatTable[cacheIndex].nextBlockIndex == NOT_USED_FLAG)
            {
                return cacheIndex;
            }
        }
    }

    // If not empty entry found
    return -1;
}

int findAvailableOpenFileTableIndex()
{
    for (int i = 0; i < MAX_NOF_OPEN_FILES; i++)
    {
        if (openFileTable[i].dirBlock == -1)
        {
            return i;
        }
    }

    return -1;
}

int allocateBlockFatEntry(int cacheIndex, int data)
{
    // Allocate the FAT Entry on memory cache
    cachedFatTable[cacheIndex].nextBlockIndex = data;

    // Allocate the FAT Entry on virtual disk
    int fatBlock = cacheIndex / FAT_ENTRY_PER_BLOCK;
    int fatBlockOffset = cacheIndex % FAT_ENTRY_PER_BLOCK;
    char block[BLOCKSIZE];
    read_block((void *)block, FAT_BLOCK_START + fatBlock);
    ((int *)(block + fatBlockOffset * FAT_ENTRY_SIZE))[0] = data;
    write_block((void *)block, FAT_BLOCK_START + fatBlock);

    // printf("LOG(allocateBlockFatEntry) (block no: %d) free block count: %d\n", cacheIndex, free_block_count);
    return cacheIndex;
};

int allocateAvailableBlockForFile(int startBlock)
{
    int lastBlock = getLastBlockOfFile(startBlock);
    int newBlock = findAvailableBlockIndex();

    if (newBlock == -1)
    {
        printf("ERROR: Block can not be allocated!\n");
        return -1;
    }

    // Allocate new block
    allocateBlockFatEntry(newBlock, EOF_FLAG);

    // Decrement free block count
    freeBlockCount--;

    // Allocate new blocks addres to last blocks value
    allocateBlockFatEntry(lastBlock, newBlock);

    // printf("LOG(allocateAvailableBlockForFile) last block: %d new block: %d free block: %d\n", lastBlock, newBlock, freeBlockCount);

    return newBlock;
};

int allocateDirectoryEntry(int cacheIndex, char *filename, int size, int startBlock, int allocationStatus)
{
    // Allocate entry on cachedRootDirectory
    memcpy(cachedRootDirectory[cacheIndex].filename, filename, MAX_FILENAME_LENGTH);
    cachedRootDirectory[cacheIndex].size = size;
    cachedRootDirectory[cacheIndex].startBlock = startBlock;
    cachedRootDirectory[cacheIndex].allocated = allocationStatus;

    // Allocation entry on virtual disk Directory Root
    int availableEntryBlock = cacheIndex / DIR_ENTRY_PER_BLOCK;
    int availableEntryBlockOffset = cacheIndex % DIR_ENTRY_PER_BLOCK;
    char block[BLOCKSIZE];
    read_block((void *)block, availableEntryBlock + ROOT_DIR_START);
    int emptyBlockOffsetBytes = availableEntryBlockOffset * DIR_ENTRY_SIZE;
    memcpy(((char *)(block + emptyBlockOffsetBytes)), filename, MAX_FILENAME_LENGTH);
    ((int *)(block + emptyBlockOffsetBytes + MAX_FILENAME_LENGTH))[0] = size;                   // size of the file
    ((int *)(block + emptyBlockOffsetBytes + (MAX_FILENAME_LENGTH + 4)))[0] = startBlock;       // startBlock
    ((int *)(block + emptyBlockOffsetBytes + (MAX_FILENAME_LENGTH + 8)))[0] = allocationStatus; // mark as used

    int res = write_block((void *)block, availableEntryBlock + ROOT_DIR_START);
    if (res == -1)
    {
        printf("ERROR: Write issue!\n");
        return -1;
    }

    return cacheIndex;
}

void allocateOpenFileTableEntry(int fd, int cacheIndex, int accessMode)
{
    openFileTable[fd].dirBlock = cacheIndex / DIR_ENTRY_PER_BLOCK;
    openFileTable[fd].dirBlockOffset = cacheIndex % DIR_ENTRY_PER_BLOCK;
    openFileTable[fd].accessMode = accessMode;
    openFileTable[fd].positionPtr = 0;
    openFileTable[fd].cachedRootDirIndex = cacheIndex;
    openFileCount++;
}

int readFromBlockToBuffer(char *blockBuffer, int block, int startOffset, int endOffset, int *byteCounter)
{
    char blockData[BLOCKSIZE];
    read_block((void *)blockData, block);
    for (int i = startOffset; i < endOffset; i++)
    {
        ((char *)(blockBuffer + *byteCounter))[0] = ((char *)(blockData + i))[0];
        (*byteCounter)++;
    }

    return *byteCounter;
}

int writeFromBufferToBlock(char *blockBuffer, int block, int startOffset, int endOffset, int *byteCounter, int writeSize)
{
    char blockData[BLOCKSIZE];
    read_block((void *)blockData, block);
    for (int i = startOffset; i < endOffset; i++)
    {
        ((char *)(blockData + i))[0] = ((char *)(blockBuffer + *byteCounter))[0];
        (*byteCounter)++;

        if (*byteCounter == writeSize)
        {
            // Early termination (found)
            write_block((void *)blockData, block);
            return *byteCounter;
        }
    }
    write_block((void *)blockData, block);
    return *byteCounter;
}

void setRootDirectoryEntry(int fd)
{
    char block[BLOCKSIZE];
    // Get the directory entry for file
    read_block((void *)block, openFileTable[fd].dirBlock + ROOT_DIR_START);
    int byteBlockOffset = openFileTable[fd].dirBlockOffset * DIR_ENTRY_SIZE;

    // Get cached root dir entry
    struct dirEntry *tmpDirEntry = &(cachedRootDirectory[openFileTable[fd].cachedRootDirIndex]);

    ((int *)(block + byteBlockOffset + MAX_FILENAME_LENGTH))[0] = tmpDirEntry->size;
    ((int *)(block + byteBlockOffset + (MAX_FILENAME_LENGTH + 4)))[0] = tmpDirEntry->startBlock;
    ((int *)(block + byteBlockOffset + (MAX_FILENAME_LENGTH + 8)))[0] = USED_FLAG;

    write_block((void *)block, openFileTable[fd].dirBlock + ROOT_DIR_START);
};

int getLastBlockOfFile(int startBlock)
{
    int traverseBlock = startBlock;
    while (cachedFatTable[traverseBlock].nextBlockIndex != EOF_FLAG)
    {
        traverseBlock = cachedFatTable[traverseBlock].nextBlockIndex;
    }

    return traverseBlock;
}

void deallocateFatEntriesOfFile(int startBlock)
{
    int traverseBlock = startBlock;
    int fatBlock;
    int fatBlockOffset;
    while (traverseBlock != EOF_FLAG)
    {
        // Deallocate FAT entry on memory cache
        int tmpNextBlock = cachedFatTable[traverseBlock].nextBlockIndex;
        cachedFatTable[traverseBlock].nextBlockIndex = NOT_USED_FLAG;

        // Deallocate FAT entry on virtual disk
        fatBlock = traverseBlock / FAT_ENTRY_PER_BLOCK;
        fatBlockOffset = traverseBlock % FAT_ENTRY_PER_BLOCK;
        char block[BLOCKSIZE];
        read_block((void *)block, FAT_BLOCK_START + fatBlock);
        ((int *)(block + FAT_ENTRY_SIZE * fatBlockOffset))[0] = NOT_USED_FLAG;
        write_block((void *)block, FAT_BLOCK_START + fatBlock);

        freeBlockCount++;
        traverseBlock = tmpNextBlock;
    }
}

void deallocateDirectoryEntry(int cacheIndex)
{
    char block[BLOCKSIZE];
    struct dirEntry *tmpDirEntry = &(cachedRootDirectory[cacheIndex]);

    // Mark directory entry available in memory cache
    tmpDirEntry->allocated = NOT_USED_FLAG;

    int dirBlock = cacheIndex / DIR_ENTRY_PER_BLOCK;
    int dirBlockOffset = cacheIndex % DIR_ENTRY_PER_BLOCK;

    // Mark directory entry available in virtual disk
    read_block((void *)block, ROOT_DIR_START + dirBlock);
    ((int *)(block + dirBlockOffset * DIR_ENTRY_SIZE + (MAX_FILENAME_LENGTH + 8)))[0] = NOT_USED_FLAG;
    write_block((void *)block, ROOT_DIR_START + dirBlock);
}