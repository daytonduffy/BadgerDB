/**
 * @author See Contributors.txt for code contributors and overview of BadgerDB.
 *
 * @section LICENSE
 * Copyright (c) 2012 Database Group, Computer Sciences Department, University of Wisconsin-Madison.
 */

#include <memory>
#include <iostream>
#include "buffer.h"
#include "exceptions/buffer_exceeded_exception.h"
#include "exceptions/page_not_pinned_exception.h"
#include "exceptions/page_pinned_exception.h"
#include "exceptions/bad_buffer_exception.h"
#include "exceptions/hash_not_found_exception.h"

namespace badgerdb
{

/**
 * Constructor of BufMgr class
 * Allocates an array for the buffer pool with bufs page frames and a corresponding BufDesc table
 */
BufMgr::BufMgr(std::uint32_t bufs)
	: numBufs(bufs)
{
	bufDescTable = new BufDesc[bufs];

	for (FrameId i = 0; i < bufs; i++)
	{
		bufDescTable[i].frameNo = i;
		bufDescTable[i].valid = false;
	}

	bufPool = new Page[bufs];

	int htsize = ((((int)(bufs * 1.2)) * 2) / 2) + 1;
	hashTable = new BufHashTbl(htsize); // allocate the buffer hash table

	clockHand = bufs - 1;
}

/**
 * Destructor of BufMgr class
 * Flushes out all dirty pages and deallocates the buffer pool and the BufDesc table
 */
BufMgr::~BufMgr()
{
	// flushes out all dirty pages
	for (FrameId i = 0; i < numBufs; i++)
	{
		//TODO: whether check bufDescTable[clockHand].valid
		if (bufDescTable[i].dirty && bufDescTable[i].valid)
		{
			bufDescTable[i].file->writePage(bufPool[i]);
			bufDescTable[i].dirty = false;
		}
	}

	//deallocates the buffer pool
	delete[] bufPool;

	//deallocates the BufDesc table
	delete[] bufDescTable;
}

/**
 * Advance clock to next frame in the buffer pool
 * 
 * @param frame   	Frame reference, frame ID of allocated frame returned via this variable
 * @throws BufferExceededException If no such buffer is found which can be allocated
 */
void BufMgr::advanceClock()
{
	clockHand = (clockHand + 1) % numBufs;
}

/**
 * Allocates a free frame
 * if necessary, writing a dirty page back to disk
 */
void BufMgr::allocBuf(FrameId &frame)
{
	//using the clock algorithm
	std::uint32_t pinned = 0;
	advanceClock();

	while (bufDescTable[clockHand].valid)
	{
		if (bufDescTable[clockHand].refbit)
		{
			//clear refbit and advance clock
			bufDescTable[clockHand].refbit = false;
			advanceClock();
			continue;
		}

		if (bufDescTable[clockHand].pinCnt)
		{
			//Throws BufferExceededException if all buffer frames are pinned.
			pinned = pinned + 1;
			if (pinned == numBufs)
				throw BufferExceededException();

			advanceClock();
			continue;
		}

		//have a valid, not referenced, and unpinned page
		//use this frame
		if (bufDescTable[clockHand].dirty)
		{
			//flush page to disk
			bufDescTable[clockHand].file->writePage(bufPool[clockHand]);
			bufDescTable[clockHand].dirty = false;
		}
		//if the buffer frame allocated has a valid page in it, remove the appropriate entry from the hash table.
		hashTable->remove(bufDescTable[clockHand].file, bufDescTable[clockHand].pageNo);
		break;
	}

	bufDescTable[clockHand].Clear();

	//set the frame ID of allocated frame returned via this variable
	//Set() in the clock algo will be called during readPage() and allocPage()
	frame = clockHand;
}

/**
 * Reads the given page from the file into a frame and returns the pointer to page.
 * If the requested page is already present in the buffer pool pointer to that frame is returned
 * otherwise a new frame is allocated from the buffer pool for reading the page.
 *
 * @param file   	File object
 * @param PageNo  Page number in the file to be read
 * @param page  	Reference to page pointer. Used to fetch the Page object in which requested page from file is read in.
 */
void BufMgr::readPage(File *file, const PageId pageNo, Page *&page)
{
	FrameId frameId;
	try
	{
		// throws HashNotFoundException if the page entry is not found in the hash table 
		hashTable->lookup(file, pageNo, frameId);

		//page is already in the buffer pool
		bufDescTable[frameId].refbit = true;
		bufDescTable[frameId].pinCnt++;
		return;
	}
	catch (HashNotFoundException e)
	{
		//page is not in the buffer pool
		// allocate a buffer frame
		this->allocBuf(frameId);

		// read the page from disk into the buffer pool
		Page new_page = file->readPage(pageNo);

		// insert the page into the hashtable
		bufPool[frameId] = new_page;
		hashTable->insert(file, pageNo, frameId);
		bufDescTable[frameId].Set(file, pageNo);
	}

	// return a pointer to the frame via the page param
	page = &bufPool[frameId];
}

/**
 * Unpin a page from memory since it is no longer required for it to remain in memory.
 *
 * @param file   	File object
 * @param PageNo  Page number
 * @param dirty		True if the page to be unpinned needs to be marked dirty	
 * @throws  PageNotPinnedException If the page is not already pinned
 */
void BufMgr::unPinPage(File *file, const PageId pageNo, const bool dirty)
{
	try
	{
		FrameId framdId;
		hashTable->lookup(file, pageNo, framdId);

		// Throws PAGENOTPINNED if the pin count is already 0
		if (!bufDescTable[framdId].pinCnt)
		{
			throw PageNotPinnedException(file->filename(), pageNo, framdId);
		}

		--bufDescTable[framdId].pinCnt;

		// if drity, sets the dirty bit.
		if (dirty)
		{
			bufDescTable[framdId].dirty = true;
		}
	} 
	catch (HashNotFoundException e)
	{
	}
}

/**
 * Writes out all dirty pages of the file to disk.
 * All the frames assigned to the file need to be unpinned from buffer pool before this function can be successfully called.
 * Otherwise Error returned.
 *
 * @param file   	File object
 * @throws PagePinnedException If any page of the file is pinned in the buffer pool 
 * @throws BadBufferException If any frame allocated to the file is found to be invalid
 */
void BufMgr::flushFile(const File *file)
{
	for (FrameId i = 0; i < numBufs; i++)
	{
		// scan bufTable for pages belonging to the file
		if (bufDescTable[i].file == file)
		{
			// throws PagePinnedException if any page of the file is pinned in the buffer pool 
			if (bufDescTable[i].pinCnt)
			{
				throw PagePinnedException(file->filename(), bufDescTable[i].pageNo, bufDescTable[i].frameNo);
			}
			
			//throws BadBufferException f any frame allocated to the file is found to be invalid
			if(bufDescTable[i].valid)
			{
				throw BadBufferException(bufDescTable[i].frameNo, bufDescTable[i].dirty, bufDescTable[i].valid, bufDescTable[i].refbit);
			}

			//if the page is dirty, flush the page to disk and then set the dirty bit
			if(bufDescTable[i].dirty) {
				bufDescTable[i].file->writePage(bufPool[i]);
				bufDescTable[i].dirty = false;
			}

			//remove the page from the hashtable 
			hashTable->remove(file, bufDescTable[i].pageNo);

			//invoke the Clear() method of BufDesc for the page frame
			bufDescTable[i].Clear();			
		}
	}
}

/**
 * Allocates a new, empty page in the file and returns the Page object.
 * The newly allocated page is also assigned a frame in the buffer pool.
 *
 * @param file   	File object
 * @param PageNo  Page number. The number assigned to the page in the file is returned via this reference.
 * @param page  	Reference to page pointer. The newly allocated in-memory Page object is returned via this reference.
 */
void BufMgr::allocPage(File *file, PageId &pageNo, Page *&page)
{
	// allocate an empty page
	Page new_page = file->allocatePage();

	// obtain a buffer pool frame
	FrameId frameId = 0;
	this->allocBuf(frameId);

	//inserted into the hash table and set the frame up
	bufPool[frameId] = new_page;
	hashTable->insert(file, pageNo, frameId);
	bufDescTable[frameId].Set(file, pageNo);

	//returns the page number via the pageNo and a pointer to the buffer frame via the page
	pageNo = new_page.page_number();
	page = &bufPool[frameId]; 
}

/**
 * Delete page from file and also from buffer pool if present.
 * Since the page is entirely deleted from file, its unnecessary to see if the page is dirty.
 *
 * @param file   	File object
 * @param PageNo  Page number
 */
void BufMgr::disposePage(File *file, const PageId PageNo)
{
	try
	{
		// if the page to be deleted is allocated a frame in the buffer pool,
		// that frame is freed and correspondingly entry from hash table is also removed.
		FrameId frameNo; // of the (file, pageNo)
		hashTable->lookup(file, PageNo, frameNo);

		hashTable->remove(file, PageNo);
		bufDescTable[frameNo].Clear();
	}
	catch (HashNotFoundException e)
	{
		//the (file, pageNo) is not in the buffer pool, do nothing
	}

	// delete the page from file
	file->deletePage(PageNo);
}

/**
 * Print member variable values. 
 */
void BufMgr::printSelf(void)
{
	BufDesc *tmpbuf;
	int validFrames = 0;

	for (std::uint32_t i = 0; i < numBufs; i++)
	{
		tmpbuf = &(bufDescTable[i]);
		std::cout << "FrameNo:" << i << " ";
		tmpbuf->Print();

		if (tmpbuf->valid == true)
			validFrames++;
	}

	std::cout << "Total Number of Valid Frames:" << validFrames << "\n";
}

} // namespace badgerdb
