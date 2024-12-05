// filehdr.cc
//	Routines for managing the disk file header (in UNIX, this
//	would be called the i-node).
//
//	The file header is used to locate where on disk the
//	file's data is stored.  We implement this as a fixed size
//	table of pointers -- each entry in the table points to the
//	disk sector containing that portion of the file data
//	(in other words, there are no indirect or doubly indirect
//	blocks). The table size is chosen so that the file header
//	will be just big enough to fit in one disk sector,
//
//      Unlike in a real system, we do not keep track of file permissions,
//	ownership, last modification date, etc., in the file header.
//
//	A file header can be initialized in two ways:
//	   for a new file, by modifying the in-memory data structure
//	     to point to the newly allocated data blocks
//	   for a file already on disk, by reading the file header from disk
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.

#include "copyright.h"

#include "filehdr.h"
#include "debug.h"
#include "synchdisk.h"
#include "main.h"

void LinkedDataSector::Debug()  {
	if (next == NULL) {
		DEBUG(dbgFile, linkSector << "{ " << dataSectors[0] << ", " << dataSectors[1] << "... }" << " -> end\n");
	} 
	else {
		DEBUG(dbgFile, linkSector << "{ " << dataSectors[0] << ", " << dataSectors[1] << "... }" << " -> ");
		next->Debug();
	}
}

//----------------------------------------------------------------------
// MP4 mod tag
// FileHeader::FileHeader
//	There is no need to initialize a fileheader,
//	since all the information should be initialized by Allocate or FetchFrom.
//	The purpose of this function is to keep valgrind happy.
//----------------------------------------------------------------------
FileHeader::FileHeader()
{
	numBytes = -1;
	numSectors = -1;
	dataSectorList = NULL;
}

//----------------------------------------------------------------------
// MP4 mod tag
// FileHeader::~FileHeader
//	Currently, there is not need to do anything in destructor function.
//	However, if you decide to add some "in-core" data in header
//	Always remember to deallocate their space or you will leak memory
//----------------------------------------------------------------------
FileHeader::~FileHeader()
{
	LinkedDataSector *curr = dataSectorList;
	while (curr != NULL) {
		LinkedDataSector *next = curr->Next();
		delete curr;
		curr = next;
	}
}

//----------------------------------------------------------------------
// FileHeader::Allocate
// 	Initialize a fresh file header for a newly created file.
//	Allocate data blocks for the file out of the map of free disk blocks.
//	Return FALSE if there are not enough free blocks to accomodate
//	the new file.
//
//	"freeMap" is the bit map of free disk sectors
//	"fileSize" is the bit map of free disk sectors
//----------------------------------------------------------------------

bool FileHeader::Allocate(PersistentBitmap *freeMap, int fileSize)
{
	numBytes = fileSize;
	numSectors = divRoundUp(fileSize, SectorSize);
	if (freeMap->NumClear() < numSectors)
		return FALSE; // not enough space

	dataSectorListFront = -1;
	LinkedDataSector *curr = NULL;
	int assigned = 0;
	while (assigned < numSectors) {
		if (dataSectorListFront == -1) {
			dataSectorListFront = freeMap->FindAndSet();
			curr = dataSectorList = new LinkedDataSector;
		} else {
			curr = curr->Push(freeMap->FindAndSet());
		}
		for (int idx = 0; idx < LinkedDirect && assigned < numSectors; idx++, assigned++) {
			curr->AssignSector(idx, freeMap->FindAndSet());
		}
		DEBUG(dbgFile, "Assign sector #" << (curr->Link() == -1 ? dataSectorListFront : curr->Link()) << " to #" << assigned / LinkedDirect + 1 << " item in linked list");
	}
	curr->Debug();
	return TRUE;
}

//----------------------------------------------------------------------
// FileHeader::Deallocate
// 	De-allocate all the space allocated for data blocks for this file.
//
//	"freeMap" is the bit map of free disk sectors
//----------------------------------------------------------------------

void FileHeader::Deallocate(PersistentBitmap *freeMap)
{
	if (dataSectorListFront == -1) return;

	int targetSector = dataSectorListFront;
	LinkedDataSector *curr = dataSectorList;
	do {
		for (int i = 0; i < LinkedDirect && curr->Data(i) != -1; i++) {
			freeMap->Clear(curr->Data(i));
		}
		freeMap->Clear(targetSector);
		targetSector = curr->Link();
		curr = curr->Next();
	} while (targetSector != -1);
}

//----------------------------------------------------------------------
// FileHeader::FetchFrom
// 	Fetch contents of file header from disk.
//
//	"sector" is the disk sector containing the file header
//----------------------------------------------------------------------

void LinkedDataSector::ReadFromSector(int sector) {
	char buf[SectorSize];
	kernel->synchDisk->ReadSector(sector, buf);

	memcpy(&linkSector, buf, sizeof(int));
	memcpy(&dataSectors, buf + sizeof(int), LinkedDirect * sizeof(int));
}

void FileHeader::FetchFrom(int sector)
{
	char buf[SectorSize];
	kernel->synchDisk->ReadSector(sector, buf);
	
	int offset = 0;
	memcpy(&numBytes, buf, sizeof(int));
	offset += sizeof(numBytes);
	memcpy(&numSectors, buf + offset, sizeof(int));
	offset += sizeof(numSectors);

	memcpy(&dataSectorListFront, buf + offset, sizeof(int));
	LinkedDataSector *curr = dataSectorList = new LinkedDataSector;
	curr->ReadFromSector(dataSectorListFront);
	
	int link;
	while ((link = curr->Link()) != -1) {
		curr = curr->Push(link);
		curr->ReadFromSector(link);
	}

	/*
		MP4 Hint:
		After you add some in-core informations, you will need to rebuild the header's structure
	*/
}

//----------------------------------------------------------------------
// FileHeader::WriteBack
// 	Write the modified contents of the file header back to disk.
//
//	"sector" is the disk sector to contain the file header
//----------------------------------------------------------------------

void LinkedDataSector::WriteBackSector(int sector) {
	char buf[SectorSize];
	memcpy(buf, &linkSector, sizeof(int));
	memcpy(buf + sizeof(int), &dataSectors, LinkedDirect * sizeof(int));
	
	kernel->synchDisk->WriteSector(sector, buf);
}

void FileHeader::WriteBack(int sector)
{
	char buf[SectorSize];

	int offset = 0;
	memcpy(buf, &numBytes, sizeof(int));
	offset += sizeof(numBytes);
	memcpy(buf + offset, &numSectors, sizeof(int));
	offset += sizeof(numSectors);
	memcpy(buf + offset, &dataSectorListFront, sizeof(int));
    kernel->synchDisk->WriteSector(sector, buf);

	if (dataSectorListFront == -1) return;
	
	LinkedDataSector *curr = dataSectorList;
	curr->WriteBackSector(dataSectorListFront);
	
	int link;
	while ((link = curr->Link()) != -1) {
		curr->WriteBackSector(link);
		curr = curr->Next();
	}

	/*
		MP4 Hint:
		After you add some in-core informations, you may not want to write all fields into disk.
		Use this instead:
		char buf[SectorSize];
		memcpy(buf + offset, &dataToBeWritten, sizeof(dataToBeWritten));
		...
	*/
}

//----------------------------------------------------------------------
// FileHeader::ByteToSector
// 	Return which disk sector is storing a particular byte within the file.
//      This is essentially a translation from a virtual address (the
//	offset in the file) to a physical address (the sector where the
//	data at the offset is stored).
//
//	"offset" is the location within the file of the byte in question
//----------------------------------------------------------------------

int FileHeader::ByteToSector(int offset)
{
	//return (dataSectors[offset / SectorSize]);
	int numSectors = offset / SectorSize;
	LinkedDataSector *target = dataSectorList;
	return target->Data(numSectors % LinkedDirect);
}

//----------------------------------------------------------------------
// FileHeader::FileLength
// 	Return the number of bytes in the file.
//----------------------------------------------------------------------

int FileHeader::FileLength()
{
	return numBytes;
}

//----------------------------------------------------------------------
// FileHeader::Print
// 	Print the contents of the file header, and the contents of all
//	the data blocks pointed to by the file header.
//----------------------------------------------------------------------

void FileHeader::Print()
{
	int i, j, k;
	char *data = new char[SectorSize];

	printf("FileHeader contents.  File size: %d.  File blocks:\n", numBytes);
	for (i = 0; i < numSectors; i++)
		printf("%d ", dataSectors[i]);
	printf("\nFile contents:\n");
	for (i = k = 0; i < numSectors; i++)
	{
		kernel->synchDisk->ReadSector(dataSectors[i], data);
		for (j = 0; (j < SectorSize) && (k < numBytes); j++, k++)
		{
			if ('\040' <= data[j] && data[j] <= '\176') // isprint(data[j])
				printf("%c", data[j]);
			else
				printf("\\%x", (unsigned char)data[j]);
		}
		printf("\n");
	}
	delete[] data;
}
