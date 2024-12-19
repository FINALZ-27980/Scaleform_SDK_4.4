/**************************************************************************

Filename    :   SocketBuffer.h
Authors     :   Alex Mantzaris

Copyright   :   Copyright 2012 Autodesk, Inc. All Rights reserved.

Use of this software is subject to the terms of the Autodesk license
agreement provided at the time of installation or download, or which
otherwise accompanies this software in either electronic or hard copy form.

**************************************************************************/

#ifndef INCLUDE_GFX_AMP_STREAM_H
#define INCLUDE_GFX_AMP_STREAM_H

#include "Kernel/SF_File.h"
#include "GFx/GFx_PlayerStats.h"

namespace Scaleform {
namespace GFx {
namespace AS3 {

// Custom stream class used to send messages across the network
// The size of each message precedes the data so we can split messages into 
// packets and reconstruct them after they have been received on the other end
//
// The data are stored as a Little-endian array of bytes
// The object normally holds only one message for write operations
// because the first four bytes are used to hold the message size
// For read operations, multiple messages can be contained on the stream
// After a message has been processed, PopFirstMessage is called to remove it
class SocketBuffer : public File
{
public:
    // File virtual method overrides
    virtual const char* GetFilePath()       { return "AS3 socket buffer"; } 
    virtual bool        IsValid() const     { return true; }
    virtual bool        IsWritable() const  { return true; }
    virtual int         Tell ()             { return readPosition; }
    virtual SInt64      LTell ()            { return  Tell(); }
    virtual int         GetLength ()        { return static_cast<int>(Data.GetSize()); }
    virtual SInt64      LGetLength ()       { return GetLength(); }
    virtual int         GetErrorCode()      { return 0; }
    virtual bool        Flush()             { return true; }
    virtual bool        Close()             { return false; }
    virtual int         Write(const UByte *pbufer, int numBytes);
    virtual int         Read(UByte *pbufer, int numBytes);
    virtual int         SkipBytes(int numBytes);                     
    virtual int         BytesAvailable();
    virtual int         Seek(int offset, int origin=Seek_Set);
    virtual int         SeekToBegin();
    virtual SInt64      LSeek(SInt64 offset, int origin=Seek_Set);
    virtual bool        ChangeSize(int newSize);
    virtual int         CopyFromStream(File *pstream, int byteSize);

    // Serialization
    void                Read(File& str);
    void                Write(File& str) const;

    // Append an buffer that is already in the AmpStream format
    void                Append(const UByte* buffer, UPInt bufferSize);

    // Data accessors
    const UByte*        GetBuffer() const;
    UPInt               GetBufferSize() const;

    // Clear the stream
    void                Clear();

    void                DiscardReadBytes();
    int                 GetReadPosition();

private:
    typedef ArrayConstPolicy<0, 4, true> NeverShrinkPolicy;
    ArrayLH<UByte, StatMV_Other_Mem, NeverShrinkPolicy>     Data;
    int                                                     readPosition;  // next location to be read
};

} // namespace AS3
} // namespace GFx
} // namespace Scaleform


#endif