//////////
//
//	File:		QTFileTransfer.c
//
//	Contains:	Sample code for transferring a file asynchronously from a web server.
//
//	Written by:	Tim Monroe
//
//	Copyright:	� 1998-1999 by Apple Computer, Inc., all rights reserved.
//
//	Change History (most recent first):
//
//	   <8>	 	07/20/00	rtm		changed return type of completion routines to PASCAL_RTN void
//	   <7>	 	03/21/00	rtm		made changes to run under CarbonLib
//	   <6>	 	10/01/99	rtm		made gDataBuffer global, so we can dispose of it in the
//									QTFileTrans_CloseDownHandlers function; added calls to
//									DisposeRoutineDescriptor
//	   <5>	 	03/03/99	rtm		switched to using DataHReadAsync; removed unnecessary NOTES
//	   <4>	 	12/25/99	rtm		added NOTES concerning DataHReadAsync and DataHGetFileSize
//	   <3>	 	11/30/98	rtm		modified code to use GetDataHandler instead of FindNextComponent;
//									moved call to QTFileTrans_CloseDownHandlers out of completion proc
//	   <2>	 	11/16/98	rtm		got asynchronous ftp and http file transfer working
//	   <1>	 	11/11/98	rtm		first file
//	 
//	QuickTime Streaming has ftp and http data handlers, which you can use to transfer files
//	synchronously or asynchronously from a web server. This sample code illustrates how to
//	perform asynchronous transfers. In all likelihood, you'll want to transfer asynchronously,
//	since your application can continue to operate while the transfer is underway.
//	
//	The basic idea is to instantiate the URL data handler and the HFS data handler; the URL
//	data handler will be reading data from a remote ftp or http file into a buffer, and the
//	HFS data handler will be writing data from that buffer into a local file. This reading and
//	writing continues until the file is completely transferred.
//
//	To transfer a remote file to the local machine, call the QTFileTrans_CopyRemoteFileToLocalFile
//	function defined here. It does all the necessary set-up and schedules the first read request;
//	all subsequent write and read requests are scheduled by the read and write completion routines.
//	Note that, when doing asynchronous transfers, you need to give time to the data handlers by
//	calling DataHTask periodically; on the Mac, you can put code like this in your main event loop:
//
//		// if we're done, close down the data handlers
//		if (gDoneTransferring)
//			QTFileTrans_CloseDownHandlers();
//
//		// give the data handlers some time, if they are still active
//		if (gDataReader != NULL)
//			DataHTask(gDataReader);
//		
//		if (gDataWriter != NULL)
//			DataHTask(gDataWriter);
//	
//	On Windows, you could install a timer that calls this code at a specified interval. (On either
//	platform, you should probably also implement some way of making sure that the user doesn't quit
//	the application while a transfer is underway.)
//
//	NOTES:
//
//	*** (1) ***
//	For information about the main routines used here, see the chapter "Data Handler Components" in
//	the document QT3.0Reference.pdf.
//
//	*** (2) ***
//	The code for implementing synchronous transfers is actually much simpler: you don't need any
//	completion routines, and the "scheduling" is much easier. Here's an outline of all you need to do
//	to transfer a file synchronously:
//
//		DataHGetData(gDataReader, gDataBuffer, 0L, 0L, kDataBufferSize);					
//		DataHCloseForRead(gDataReader);
//		DataHPutData(gDataWriter, gDataBuffer, 0L, NULL, kDataBufferSize);
//		DataHCloseForWrite(gDataWriter);	
//
//	In this case, however, gDataBuffer is a handle, not a pointer. (Also, we've assumed that the file
//	being transferred fits completely into the buffer; you could easily fix that assumption.)
//
//	*** (3) ***
//	You'll notice that we use our completion routines to schedule subsequent data reads and writes.
//	This is okay because data handler completion routines are never called at interrupt time.
//
//	*** (4) ***
//	In some instances, DataHGetFileSize is not able to determine the size of the file to be downloaded
//	(for example, an FTP server might not support the SIZE command). A more general strategy therefore
//	would be to download a file until you get eofErr. Implementing this strategy is left as an exercise
//	for the reader.
//
//////////


#include "QTFileTransfer.h"

//global variables
Ptr								gDataBuffer = NULL;			// buffer that holds data being transferred
ComponentInstance				gDataReader = NULL;			// the data handler that reads data from the URL
ComponentInstance				gDataWriter = NULL;			// the data handler that writes data to an HFS file
DataHCompletionUPP				gReadDataHCompletionUPP = NULL;
DataHCompletionUPP				gWriteDataHCompletionUPP = NULL;
long							gBytesToTransfer = 0L;		// the number of bytes to transfer
long							gBytesTransferred = 0L;		// the number of bytes already transferred
Boolean							gDoneTransferring = false;	// are we done transferring data?


//////////
//
// QTFileTrans_CopyRemoteFileToLocalFile
// Copy a remote file (located at the specified URL) into a local file.
//
//////////

OSErr QTFileTrans_CopyRemoteFileToLocalFile (char *theURL, FSSpecPtr theFSSpecPtr)
{
	Handle						myReaderRef = NULL;			// data reference for the remote file
	Handle						myWriterRef = NULL;			// data reference for the local file
	Size						mySize = 0;
	ComponentResult				myErr = badComponentType;

	//////////
	//
	// create a data reference for the remote file
	//
	//////////
	
	// get the size of the URL, plus the terminating null byte
	mySize = (Size)strlen(theURL) + 1;
	if (mySize == 0)
		goto bail;
	
	// allocate a new handle
	myReaderRef = NewHandleClear(mySize);
    if (myReaderRef == NULL)
    	goto bail;

	// copy the URL into the handle
	BlockMove(theURL, *myReaderRef, mySize);

	//////////
	//
	// create a data reference for the local file
	//
	//////////
	
	// delete the target local file, if it already exists;
	// if it doesn't exist yet, we'll get an error (fnfErr), which we just ignore
	FSpDelete(theFSSpecPtr);
	
	myWriterRef = NewHandleClear(sizeof(Handle));
    if (myWriterRef == NULL)
    	goto bail;

	// create the local file
	myErr = FSpCreate(theFSSpecPtr, kTransFileCreator, kTransFileType, smSystemScript);
	if (myErr != noErr)
		goto bail;

	myErr = QTNewAlias(theFSSpecPtr, (AliasHandle *)&myWriterRef, true);
	if (myErr != noErr)
		goto bail;

	//////////
	//
	// find and open the Apple URL and HFS data handlers; connect the data references to them
	//
	//////////
	
	gDataReader = OpenComponent(GetDataHandler(myReaderRef, URLDataHandlerSubType, kDataHCanRead));
	if (gDataReader == NULL)
		goto bail;

	gDataWriter = OpenComponent(GetDataHandler(myWriterRef, rAliasType, kDataHCanWrite));
	if (gDataWriter == NULL)
		goto bail;
		
	// set the data reference for the URL data handler
	myErr = DataHSetDataRef(gDataReader, myReaderRef);
	if (myErr != noErr)
		goto bail;
	
	// set the data reference for the HFS data handler
	myErr = DataHSetDataRef(gDataWriter, myWriterRef);
	if (myErr != noErr)
		goto bail;
	
	//////////
	//
	// allocate a data buffer; the URL data handler copies data into this buffer,
	// and the HFS data handler copies data out of it
	//
	//////////
	
	gDataBuffer = NewPtrClear(kDataBufferSize);
	myErr = MemError();
	if (myErr != noErr)
		goto bail;
		
	//////////
	//
	// connect to the remote and local files
	//
	//////////
	
	// open a read-only path to the remote data reference
	myErr = DataHOpenForRead(gDataReader);
	if (myErr != noErr)
		goto bail;

	// get the size of the remote file
	myErr = DataHGetFileSize(gDataReader, &gBytesToTransfer); 
	if (myErr != noErr)
		goto bail;
	
	// open a write-only path to the local data reference
	myErr = DataHOpenForWrite(gDataWriter);
	if (myErr != noErr)
		goto bail;
		
	//////////
	//
	// start reading and writing data
	//
	//////////
	
	gDoneTransferring = false;
	gBytesTransferred = 0L;
	
	gReadDataHCompletionUPP = NewDataHCompletionUPP(QTFileTrans_ReadDataCompletionProc);
	gWriteDataHCompletionUPP = NewDataHCompletionUPP(QTFileTrans_WriteDataCompletionProc);
		
	// start retrieving the data; we do this by calling our own write completion routine,
	// pretending that we've just successfully finished writing 0 bytes of data
	QTFileTrans_WriteDataCompletionProc(gDataBuffer, 0L, noErr);

bail:
	// if we encountered any error, close the data handler components
	if (myErr != noErr)
		QTFileTrans_CloseDownHandlers();
	
	return((OSErr)myErr);
}


//////////
//
// QTFileTrans_ReadDataCompletionProc
// This procedure is called when the data handler has completed a read operation.
//
// The theRefCon parameter contains the number of bytes just read.
//
//////////

PASCAL_RTN void QTFileTrans_ReadDataCompletionProc (Ptr theRequest, long theRefCon, OSErr theErr)
{
#pragma unused(theErr)

	// we just finished reading some data, so schedule a write operation			
	DataHWrite(	gDataWriter,
				theRequest,						// the data buffer
				gBytesTransferred,				// write from the current offset
				theRefCon,						// the number of bytes to write
				gWriteDataHCompletionUPP,
				theRefCon);
}


//////////
//
// QTFileTrans_WriteDataCompletionProc
// This procedure is called when the data handler has completed a write operation.
//
// The theRefCon parameter contains the number of bytes just written.
//
//////////

PASCAL_RTN void QTFileTrans_WriteDataCompletionProc (Ptr theRequest, long theRefCon, OSErr theErr)
{
#pragma unused(theErr)

	long		myNumBytesToRead;
	wide		myWide;

	// increment our tally of the number of bytes written so far
	gBytesTransferred += theRefCon;

	if (gBytesTransferred < gBytesToTransfer) {
		// there is still data to read and write, so schedule a read operation
	
		// determine how big a chunk to read
		if (gBytesToTransfer - gBytesTransferred > kDataBufferSize)
			myNumBytesToRead = kDataBufferSize;
		else
			myNumBytesToRead = gBytesToTransfer - gBytesTransferred;

		myWide.lo = gBytesTransferred;			// read from the current offset 
		myWide.hi = 0;
		
		// schedule a read operation
		DataHReadAsync(gDataReader,
						theRequest,				// the data buffer
						myNumBytesToRead,
						&myWide,
						gReadDataHCompletionUPP,
						myNumBytesToRead);

	} else {
		// we've transferred all the data, so set a flag to tell us to close down the data handlers
		gDoneTransferring = true;
	}
	
}


//////////
//
// QTFileTrans_CloseDownHandlers
// Close our read/write access to our data references and then close down the read/write data handlers.
//
//////////

void QTFileTrans_CloseDownHandlers (void)
{
	if (gDataReader != NULL) {
		DataHCloseForRead(gDataReader);
		CloseComponent(gDataReader);
		gDataReader = NULL;
	}

	if (gDataWriter != NULL) {
		DataHCloseForWrite(gDataWriter);
		CloseComponent(gDataWriter);
		gDataWriter = NULL;
	}
	
	// dispose of the data buffer
	if (gDataBuffer != NULL)
		DisposePtr(gDataBuffer);
		
	// dispose of the routine descriptors
	if (gReadDataHCompletionUPP != NULL)
		DisposeDataHCompletionUPP(gReadDataHCompletionUPP);
		
	if (gWriteDataHCompletionUPP != NULL)
		DisposeDataHCompletionUPP(gWriteDataHCompletionUPP);
}