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

void LinkedDataSector::FetchFromSector(int sector) {
	char buf[SectorSize];
	kernel->synchDisk->ReadSector(sector, buf);

	memcpy(&linkSector, buf, sizeof(int));
	memcpy(&dataSectors, buf + sizeof(int), LinkedDirect * sizeof(int));
	//DEBUG(dbgFile, "Read linked list from sector #" << sector << ", while next item is at sector #" << linkSector << " (-1 = end).");
}

void LinkedDataSector::WriteBackSector(int sector) {
	char buf[SectorSize];
	memcpy(buf, &linkSector, sizeof(int));
	memcpy(buf + sizeof(int), &dataSectors, LinkedDirect * sizeof(int));
	
	kernel->synchDisk->WriteSector(sector, buf);
	DEBUG(dbgFile, "Write linked list to sector #" << sector << ", while next item is at sector #" << linkSector << " (-1 = end)");
}

void LinkedDataSector::Debug()  {
	if (next == NULL) {
		cout << "{ " << dataSectors[0] << ", " << dataSectors[1] << "... }" << " -> end\n";
	} 
	else {
		cout << "{ " << dataSectors[0] << " ~ " << dataSectors[LinkedDirect - 1] << " }" << " -- " << linkSector << " -> ";
		next->Debug();
	}
}

bool SeqDataSectors::Allocate(PersistentBitmap *freeMap, int fileSize) {
	int numSectors = divRoundUp(fileSize, SectorSize);
	if (freeMap->NumClear() < numSectors) {
		cerr << "Not enough space!\n";
		return FALSE; // not enough space
	}

	LinkedDataSector *curr = NULL;
	int assigned = 0;
	while (assigned < numSectors) {
		if (front == -1) {
			front = freeMap->FindAndSet();
			ASSERT(front >= 0);
			curr = list = new LinkedDataSector;
			DEBUG(dbgFile, "Set front of the linked list, stored in sector #" << front << ".");
		} else {
			int sector = freeMap->FindAndSet();
			ASSERT(sector >= 0);
			curr = curr->Push(sector);
			DEBUG(dbgFile, "Add new item into linked list, stored in sector #" << sector << ".");
		}
		for (int idx = 0; idx < LinkedDirect && assigned < numSectors; idx++, assigned++) {
			int sector = freeMap->FindAndSet();
			ASSERT(sector >= 0);
			curr->AssignSector(idx, sector);
			DEBUG(dbgFile, "Assign sector #" << sector << " to #" << idx << " item.");
		}
	}
	if (debug->IsEnabled('f')) Debug();
	return TRUE;
}

void SeqDataSectors::Deallocate(PersistentBitmap *freeMap) {
	if (front == -1) return;

	int targetSector = front;
	LinkedDataSector *curr = list;
	do {
		for (int i = 0; i < LinkedDirect && curr->Data(i) != -1; i++) {
			freeMap->Clear(curr->Data(i));
		}
		freeMap->Clear(targetSector);
		targetSector = curr->Link();
		curr = curr->Next();
	} while (targetSector != -1);
}

void SeqDataSectors::FetchFrom(char *buf) {
	memcpy(&front, buf, sizeof(int));
	LinkedDataSector *curr = list = new LinkedDataSector;
	curr->FetchFromSector(front);
	
	if (front == -1) return;

	int link;
	while ((link = curr->Link()) != -1) {
		curr = curr->Push(link);
		curr->FetchFromSector(link);
	}
}

void SeqDataSectors::WriteBack(char *buf) {
	memcpy(buf, &front, sizeof(int));

	if (front == -1) return;
	
	LinkedDataSector *curr = list;
	curr->WriteBackSector(front);
	
	int link;
	while ((link = curr->Link()) != -1) {
		curr = curr->Next();
		curr->WriteBackSector(link);
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
	dataSectorList.Allocate(freeMap, fileSize);
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
	dataSectorList.Deallocate(freeMap);
}

//----------------------------------------------------------------------
// FileHeader::FetchFrom
// 	Fetch contents of file header from disk.
//
//	"sector" is the disk sector containing the file header
//----------------------------------------------------------------------

void FileHeader::FetchFrom(int sector)
{
	char buf[SectorSize];
	kernel->synchDisk->ReadSector(sector, buf);
	
	int offset = 0;
	memcpy(&numBytes, buf, sizeof(int));
	offset += sizeof(numBytes);
	memcpy(&numSectors, buf + offset, sizeof(int));
	offset += sizeof(numSectors);

	dataSectorList.FetchFrom(buf + offset);
}

//----------------------------------------------------------------------
// FileHeader::WriteBack
// 	Write the modified contents of the file header back to disk.
//
//	"sector" is the disk sector to contain the file header
//----------------------------------------------------------------------

void FileHeader::WriteBack(int sector)
{
	char buf[SectorSize];

	int offset = 0;
	memcpy(buf, &numBytes, sizeof(int));
	offset += sizeof(numBytes);
	memcpy(buf + offset, &numSectors, sizeof(int));
	offset += sizeof(numSectors);
	//memcpy(buf + offset, &dataSectorListFront, sizeof(int));
	dataSectorList.WriteBack(buf + offset);

    kernel->synchDisk->WriteSector(sector, buf);
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

int SeqDataSectors::GetSector(int offset) {
	int numSectors = offset / SectorSize;
	LinkedDataSector *target = list;
	for (int i = 0; i < numSectors / LinkedDirect; i++) {
		target = target->Next();
		ASSERT(target != NULL);
	}
	return target->Data(numSectors % LinkedDirect);
}

int FileHeader::ByteToSector(int offset)
{
	return dataSectorList.GetSector(offset);
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

void LinkedDataSector::Print(int numBytes) {
	int i, j, k;
	char *data = new char[SectorSize];
	numBytes = numBytes < LinkedDirect * SectorSize ? numBytes : LinkedDirect * SectorSize;
	int numSectors = divRoundUp(numBytes, SectorSize);

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
	if (numBytes > LinkedDirect) next->Print(numBytes - LinkedDirect * SectorSize);
}

void FileHeader::Print()
{
	printf("FileHeader contents.  File size: %d.  File blocks:\n", numBytes);
	dataSectorList.Print(numBytes);
}
