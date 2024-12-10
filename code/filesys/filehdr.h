// filehdr.h
//	Data structures for managing a disk file header.
//
//	A file header describes where on disk to find the data in a file,
//	along with other information about the file (for instance, its
//	length, owner, etc.)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.

#include "copyright.h"

#ifndef FILEHDR_H
#define FILEHDR_H

#include "disk.h"
#include "pbitmap.h"
#include "list.h"

#define NumDirect ((SectorSize - 2 * sizeof(int)) / sizeof(int))
#define LinkedDirect ((SectorSize - sizeof(int)) / sizeof(int))

class LinkedDataSector {
public:
	LinkedDataSector(int sector): linkSector(sector), next(NULL) {
		for (int i = 0; i < LinkedDirect; i++) dataSectors[i] = -1;
	}
	LinkedDataSector(): linkSector(-1), next(NULL) {
		for (int i = 0; i < LinkedDirect; i++) dataSectors[i] = -1;
	}
	~LinkedDataSector() { delete next; }

	int Link() { return linkSector; }
	
	int Data(int idx) { return dataSectors[idx]; }
	void AssignSector(int idx, int sector) { dataSectors[idx] = sector; }
	
	LinkedDataSector *Next() { return next; }
	LinkedDataSector *Push(int sector) { linkSector = sector; next = new LinkedDataSector; return next; }

	void Debug();
	void Print(int numBytes);

	void FetchFromSector(int sector);
	void WriteBackSector(int sector);

private:
	int linkSector;					// Both stored in disk
	int dataSectors[LinkedDirect];
	LinkedDataSector *next; // In-core member, was NULL
};

class SeqDataSectors {
public:
	SeqDataSectors(): front(-1), list(NULL) {}
	~SeqDataSectors() { delete list; }
	bool Allocate(PersistentBitmap *freeMap, int fileSize);
	void Deallocate(PersistentBitmap *freeMap);
	void FetchFrom(char *buf);
	void WriteBack(char *buf);
	int GetSector(int offset);
	void Debug() { cout << front << " -> "; list->Debug(); }
	void Print(int numBytes) { list->Print(numBytes); }
private:
	int front;
	LinkedDataSector *list;
};

// The following class defines the Nachos "file header" (in UNIX terms,
// the "i-node"), describing where on disk to find all of the data in the file.
// The file header is organized as a simple table of pointers to
// data blocks.
//
// The file header data structure can be stored in memory or on disk.
// When it is on disk, it is stored in a single sector -- this means
// that we assume the size of this data structure to be the same
// as one disk sector.  Without indirect addressing, this
// limits the maximum file length to just under 4K bytes.
//
// There is no constructor; rather the file header can be initialized
// by allocating blocks for the file (if it is a new file), or by
// reading it from disk.

class FileHeader
{
public:
	// MP4 mod tag
	FileHeader(); // dummy constructor to keep valgrind happy
	~FileHeader();

	bool Allocate(PersistentBitmap *bitMap, int fileSize); // Initialize a file header,
														   //  including allocating space
														   //  on disk for the file data
	void Deallocate(PersistentBitmap *bitMap);			   // De-allocate this file's
														   //  data blocks

	void FetchFrom(int sectorNumber); // Initialize file header from disk
	void WriteBack(int sectorNumber); // Write modifications to file header
									  //  back to disk

	int ByteToSector(int offset); // Convert a byte offset into the file
								  // to the disk sector containing
								  // the byte

	int FileLength(); // Return the length of the file
					  // in bytes

	void Print(); // Print the contents of the file.

private:
	/*
		MP4 hint:
		You will need a data structure to store more information in a header.
		Fields in a class can be separated into disk part and in-core part.
		Disk part are data that will be written into disk.
		In-core part are data only lies in memory, and are used to maintain the data structure of this class.
		In order to implement a data structure, you will need to add some "in-core" data
		to maintain data structure.
		
		Disk Part - numBytes, numSectors, dataSectors occupy exactly 128 bytes and will be
		written to a sector on disk.
		In-core part - none
		
	*/

	int numBytes;				// Number of bytes in the file
	int numSectors;				// Number of data sectors in the file
	SeqDataSectors dataSectorList;	// Disk sector numbers for each data block in the file
};

#endif // FILEHDR_H
