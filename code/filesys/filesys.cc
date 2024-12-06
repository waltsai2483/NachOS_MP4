// filesys.cc
//	Routines to manage the overall operation of the file system.
//	Implements routines to map from textual file names to files.
//
//	Each file in the file system has:
//	   A file header, stored in a sector on disk
//		(the size of the file header data structure is arranged
//		to be precisely the size of 1 disk sector)
//	   A number of data blocks
//	   An entry in the file system directory
//
// 	The file system consists of several data structures:
//	   A bitmap of free disk sectors (cf. bitmap.h)
//	   A directory of file names and file headers
//
//      Both the bitmap and the directory are represented as normal
//	files.  Their file headers are located in specific sectors
//	(sector 0 and sector 1), so that the file system can find them
//	on bootup.
//
//	The file system assumes that the bitmap and directory files are
//	kept "open" continuously while Nachos is running.
//
//	For those operations (such as Create, Remove) that modify the
//	directory and/or bitmap, if the operation succeeds, the changes
//	are written immediately back to disk (the two files are kept
//	open during all this time).  If the operation fails, and we have
//	modified part of the directory and/or bitmap, we simply discard
//	the changed version, without writing it back to disk.
//
// 	Our implementation at this point has the following restrictions:
//
//	   there is no synchronization for concurrent accesses
//	   files have a fixed size, set when the file is created
//	   files cannot be bigger than about 3KB in size
//	   there is no hierarchical directory structure, and only a limited
//	     number of files can be added to the system
//	   there is no attempt to make the system robust to failures
//	    (if Nachos exits in the middle of an operation that modifies
//	    the file system, it may corrupt the disk)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.
#ifndef FILESYS_STUB

#include "copyright.h"
#include "debug.h"
#include "disk.h"
#include "pbitmap.h"
#include "directory.h"
#include "filehdr.h"
#include "filesys.h"

// Sectors containing the file headers for the bitmap of free sectors,
// and the directory of files.  These file headers are placed in well-known
// sectors, so that they can be located on boot-up.
#define FreeMapSector 0
#define DirectorySector 1

// Initial file sizes for the bitmap and directory; until the file system
// supports extensible files, the directory size sets the maximum number
// of files that can be loaded onto the disk.
#define FreeMapFileSize (NumSectors / BitsInByte)
#define DirectoryFileSize (sizeof(DirectoryEntry) * NumDirEntries)

//----------------------------------------------------------------------
// FileSystem::FileSystem
// 	Initialize the file system.  If format = TRUE, the disk has
//	nothing on it, and we need to initialize the disk to contain
//	an empty directory, and a bitmap of free sectors (with almost but
//	not all of the sectors marked as free).
//
//	If format = FALSE, we just have to open the files
//	representing the bitmap and the directory.
//
//	"format" -- should we initialize the disk?
//----------------------------------------------------------------------

FileSystem::FileSystem(bool format)
{
    DEBUG(dbgFile, "Initializing the file system.");
    if (format)
    {
        PersistentBitmap *freeMap = new PersistentBitmap(NumSectors);
        Directory *directory = new Directory(NumDirEntries);
        FileHeader *mapHdr = new FileHeader;
        FileHeader *dirHdr = new FileHeader;

        DEBUG(dbgFile, "Formatting the file system.");

        // First, allocate space for FileHeaders for the directory and bitmap
        // (make sure no one else grabs these!)
        freeMap->Mark(FreeMapSector);
        freeMap->Mark(DirectorySector);

        // Second, allocate space for the data blocks containing the contents
        // of the directory and bitmap files.  There better be enough space!

        ASSERT(mapHdr->Allocate(freeMap, FreeMapFileSize));
        ASSERT(dirHdr->Allocate(freeMap, DirectoryFileSize));

        // Flush the bitmap and directory FileHeaders back to disk
        // We need to do this before we can "Open" the file, since open
        // reads the file header off of disk (and currently the disk has garbage
        // on it!).

        DEBUG(dbgFile, "Writing headers back to disk.");
        mapHdr->WriteBack(FreeMapSector);
        dirHdr->WriteBack(DirectorySector);

        // OK to open the bitmap and directory files now
        // The file system operations assume these two files are left open
        // while Nachos is running.

        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);

        // Once we have the files "open", we can write the initial version
        // of each file back to disk.  The directory at this point is completely
        // empty; but the bitmap has been changed to reflect the fact that
        // sectors on the disk have been allocated for the file headers and
        // to hold the file data for the directory and bitmap.

        DEBUG(dbgFile, "Writing bitmap and directory back to disk.");
        freeMap->WriteBack(freeMapFile); // flush changes to disk
        directory->WriteBack(directoryFile);

        if (debug->IsEnabled('f'))
        {
            freeMap->Print();
            directory->Print();
        }
        delete freeMap;
        delete directory;
        delete mapHdr;
        delete dirHdr;
    }
    else
    {
        // if we are not formatting the disk, just open the files representing
        // the bitmap and directory; these are left open while Nachos is running
        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);
    }
}

//----------------------------------------------------------------------
// MP4 mod tag
// FileSystem::~FileSystem
//----------------------------------------------------------------------
FileSystem::~FileSystem()
{
    delete openedFile;
    delete freeMapFile;
    delete directoryFile;
}

Path FileSystem::DescribePath(char *path) {
    int len = strlen(path);

    Path obj;
    obj.dirSector = -1;
    memset(obj.name, '\0', FileNameMaxLen * sizeof(char));
    
    char dir[len];
    memset(dir, '\0', len * sizeof(char));
    int dirEnd = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            dirEnd = i;
            break;
        }
    }

    ASSERT(dirEnd >= 0);

    int count = 0;
    for (int i = 0; i < len; i++) {
        if (i == dirEnd) {
            if (dirEnd == 0) {
                dir[count++] = '/';
            }
            dir[count] = '\0';
            obj.name[0] = '/';
            count = 1;
        } else if (i < dirEnd) {
            dir[count++] = path[i];
        } else {
            obj.name[count++] = path[i];
        }
    }
    obj.name[count] = '\0';
    obj.dirSector = TraverseDirectory(dir);

    return obj;
} 


//----------------------------------------------------------------------
// FileSystem::Create
// 	Create a file in the Nachos file system (similar to UNIX create).
//	Since we can't increase the size of files dynamically, we have
//	to give Create the initial size of the file.
//
//	The steps to create a file are:
//	  Make sure the file doesn't already exist
//        Allocate a sector for the file header
// 	  Allocate space on disk for the data blocks for the file
//	  Add the name to the directory
//	  Store the new file header on disk
//	  Flush the changes to the bitmap and the directory back to disk
//
//	Return TRUE if everything goes ok, otherwise, return FALSE.
//
// 	Create fails if:
//   		file is already in directory
//	 	no free space for file header
//	 	no free entry for file in directory
//	 	no free space for data blocks for the file
//
// 	Note that this implementation assumes there is no concurrent access
//	to the file system!
//
//	"name" -- name of file to be created
//	"initialSize" -- size of file to be created
//----------------------------------------------------------------------

bool FileSystem::Create(char *name, int initialSize)
{
    Directory *directory;
    OpenFile *dirFile;
    PersistentBitmap *freeMap;
    FileHeader *hdr;
    int sector;
    bool success;

    char filename[FileNameMaxLen+1];

    Path path = DescribePath(name);
    ASSERT(path.dirSector >= 0);
    DEBUG(dbgFile, "Split path " << name << " into dir sector = " << path.dirSector << " and filename " << path.name << ".");

    directory = new Directory(NumDirEntries);
    if (path.dirSector == DirectorySector) {
        dirFile = directoryFile;
    } else {
        dirFile = new OpenFile(path.dirSector);
    }
    directory->FetchFrom(dirFile);

    if (directory->Find(path.name) != -1)
        success = FALSE; // file is already in directory
    else {
        freeMap = new PersistentBitmap(freeMapFile, NumSectors);
        sector = freeMap->FindAndSet(); // find a sector to hold the file header
        if (sector == -1)
            success = FALSE; // no free block for file header
        else if (!directory->Add(path.name, sector))
            success = FALSE; // no space in directory
        else
        {
            hdr = new FileHeader;
            if (!hdr->Allocate(freeMap, initialSize))
                success = FALSE; // no space on disk for data
            else
            {
                success = TRUE;
                // everthing worked, flush all changes back to disk
                hdr->WriteBack(sector);
                directory->WriteBack(dirFile);
                freeMap->WriteBack(freeMapFile);
            }
            delete hdr;
        }
        delete freeMap;
    }
    delete directory;
    if (dirFile != directoryFile) delete dirFile;
    return success;
}

// It will go thru every directory of the path, create one if it's not found.
int FileSystem::TraverseDirectory(char *path)
{
    char dirname[FileNameMaxLen + 1];
    memset(dirname, '\0', FileNameMaxLen * sizeof(char));

    ASSERT(path[0] == '/'); // Invalid directory format
    
    int curr = 1, idx = 0;
    Directory *currDir = new Directory(NumDirEntries);
    OpenFile *currDirFile = directoryFile;
    PersistentBitmap *freeMap = new PersistentBitmap(freeMapFile, NumSectors);
    int subdirSector;
    Directory emptyDir(NumDirEntries);  // Use an empty directory instance
                                        // as a template so that sectors of every
                                        // dirs created can be resetted using it.

    while (true) {
        if (path[curr] == '/' || path[curr] == '\0') {
            if (dirname[0] == '\0' && path[curr] == '\0') { // Root directory
                DEBUG(dbgFile, "The directory /" << dirname << " is root directory, which is in sector #" << subdirSector);
                delete freeMap;
                delete currDir; // Note that do not delete the currDirFile
                                // Because in root dir, the curr dir is directoryFile in fileSystem
                return DirectorySector;
            }
            DEBUG(dbgFile, "Create directory /" << dirname);

            currDir->FetchFrom(currDirFile);
            subdirSector = currDir->Find(dirname);

            // Subdir not found or corrupted (if invalid), must create one.
            if (subdirSector == -1) { 
                subdirSector = freeMap->FindAndSet(); // Find a sector to store dir header
                ASSERT(subdirSector >= 0);

                FileHeader *dirHdr = new FileHeader;
                ASSERT(dirHdr->Allocate(freeMap, DirectoryFileSize));

                freeMap->Mark(subdirSector);
                currDir->Add(dirname, subdirSector, TRUE);

                dirHdr->WriteBack(subdirSector);
                currDir->WriteBack(currDirFile);
                freeMap->WriteBack(freeMapFile);
                DEBUG(dbgFile, "Create directory /" << dirname << " with data stored in sector #" << subdirSector);
                
                delete dirHdr;

                currDirFile = new OpenFile(subdirSector); // Overwrite the subdir sector as an empty directory
                emptyDir.WriteBack(currDirFile);
            } else {
                currDirFile = new OpenFile(subdirSector);
                DEBUG(dbgFile, "Successfully find directory /" << dirname << " with data stored in sector #" << subdirSector);
            }

            // After creating / routing to the subdirectory,
            // the dirname should be reset.

            memset(dirname, '\0', FileNameMaxLen * sizeof(char));    
            idx = 0;
        } else {
            dirname[idx++] = path[curr];
        }
        if (path[curr] == '\0') break;
        curr++;
    }

    delete currDir;
    delete currDirFile;
    delete freeMap;
    return subdirSector;
}

//----------------------------------------------------------------------
// FileSystem::Open
// 	Open a file for reading and writing.
//	To open a file:
//	  Find the location of the file's header, using the directory
//	  Bring the header into memory
//
//	"name" -- the text name of the file to be opened
//----------------------------------------------------------------------

OpenFile * FileSystem::Open(char *name)
{
    Directory *directory = new Directory(NumDirEntries);
    OpenFile *dirFile;
    Path path = DescribePath(name);
    ASSERT(path.dirSector >= 0);

    dirFile = new OpenFile(path.dirSector);
    directory->FetchFrom(dirFile);

    int fileSector = directory->Find(path.name);
    if (fileSector == -1) {
        DEBUG(dbgFile, "File " << name << " does not exist!");
        return NULL;
    }

    DEBUG(dbgFile, "Opening file " << path.name << " in sector #" << fileSector);
    OpenFile *openFile = new OpenFile(fileSector); // name was found in directory
    delete directory;
    delete dirFile;
    return openFile; // return NULL if not found
}

OpenFileId FileSystem::OpenAndStore(char *name) {
    openedFile = Open(name);
    if (openedFile == NULL) {
        return 0; // Failed to open the file
    }
    return 1;
}

int FileSystem::Read(char *buf, int size, OpenFileId id) {
    return openedFile->Read(buf, size);
}

int FileSystem::Write(char *buf, int size, OpenFileId id) {
    return openedFile->Write(buf, size);
}

int FileSystem::Close(OpenFileId id) {
    openedFile = NULL;
    return 1;
}



//----------------------------------------------------------------------
// FileSystem::Remove
// 	Delete a file from the file system.  This requires:
//	    Remove it from the directory
//	    Delete the space for its header
//	    Delete the space for its data blocks
//	    Write changes to directory, bitmap back to disk
//
//	Return TRUE if the file was deleted, FALSE if the file wasn't
//	in the file system.
//
//	"name" -- the text name of the file to be removed
//----------------------------------------------------------------------

bool FileSystem::Remove(char *name)
{
    Directory *directory;
    PersistentBitmap *freeMap;
    FileHeader *fileHdr;
    int sector;

    directory = new Directory(NumDirEntries);
    directory->FetchFrom(directoryFile);
    sector = directory->Find(name);
    if (sector == -1)
    {
        delete directory;
        return FALSE; // file not found
    }
    fileHdr = new FileHeader;
    fileHdr->FetchFrom(sector);

    freeMap = new PersistentBitmap(freeMapFile, NumSectors);

    fileHdr->Deallocate(freeMap); // remove data blocks
    freeMap->Clear(sector);       // remove header block
    directory->Remove(name);

    freeMap->WriteBack(freeMapFile);     // flush to disk
    directory->WriteBack(directoryFile); // flush to disk
    delete fileHdr;
    delete directory;
    delete freeMap;
    return TRUE;
}

//----------------------------------------------------------------------
// FileSystem::List
// 	List all the files in the file system directory.
//----------------------------------------------------------------------

void FileSystem::List()
{
    Directory *directory = new Directory(NumDirEntries);

    directory->FetchFrom(directoryFile);
    directory->List();
    delete directory;
}

void FileSystem::ListRecursively() {
    PersistentBitmap *freeMap = new PersistentBitmap(freeMapFile, NumSectors);
    Directory *directory = new Directory(NumDirEntries);

    directory->FetchFrom(directoryFile);
    directory->ListRecursively(freeMap, 0);
    delete directory;
}

//----------------------------------------------------------------------
// FileSystem::Print
// 	Print everything about the file system:
//	  the contents of the bitmap
//	  the contents of the directory
//	  for each file in the directory,
//	      the contents of the file header
//	      the data in the file
//----------------------------------------------------------------------

void FileSystem::Print()
{
    FileHeader *bitHdr = new FileHeader;
    FileHeader *dirHdr = new FileHeader;
    PersistentBitmap *freeMap = new PersistentBitmap(freeMapFile, NumSectors);
    Directory *directory = new Directory(NumDirEntries);

    printf("Bit map file header:\n");
    bitHdr->FetchFrom(FreeMapSector);
    bitHdr->Print();

    printf("Directory file header:\n");
    dirHdr->FetchFrom(DirectorySector);
    dirHdr->Print();

    freeMap->Print();

    directory->FetchFrom(directoryFile);
    directory->Print();

    delete bitHdr;
    delete dirHdr;
    delete freeMap;
    delete directory;
}

#endif // FILESYS_STUB
