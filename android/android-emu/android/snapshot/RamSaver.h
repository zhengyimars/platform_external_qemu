// Copyright 2017 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#pragma once

#include "android/base/Compiler.h"
#include "android/base/EnumFlags.h"
#include "android/base/containers/SmallVector.h"
#include "android/base/files/StdioStream.h"
#include "android/base/synchronization/Lock.h"
#include "android/base/synchronization/MessageChannel.h"
#include "android/base/system/System.h"
#include "android/base/threads/FunctorThread.h"
#include "android/base/threads/ThreadPool.h"
#include "android/snapshot/Compressor.h"
#include "android/snapshot/FastReleasePool.h"
#include "android/snapshot/GapTracker.h"
#include "android/snapshot/IncrementalStats.h"
#include "android/snapshot/RamLoader.h"
#include "android/snapshot/common.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace android {
namespace snapshot {

using namespace ::android::base::EnumFlags;

class RamLoader;

class RamSaver {
    DISALLOW_COPY_AND_ASSIGN(RamSaver);

public:
    enum class Flags : uint8_t {
        None = 0,
        Async = 0x1,
        // TODO: add "CopyOnWrite = 0x3  // implies |Async|"
        Compress = 0x4,
    };

    RamSaver(const std::string& fileName,
             Flags preferredFlags,
             RamLoader* loader,
             bool isOnExit);
    ~RamSaver();

    void registerBlock(const RamBlock& block);
    void savePage(int64_t blockOffset, int64_t pageOffset, int32_t pageSize);
    void complete();
    void join();
    bool hasError() const { return mHasError; }
    bool compressed() const {
        return mIndex.flags & int32_t(IndexFlags::CompressedPages);
    }
    uint64_t diskSize() const { return mDiskSize; }
    bool incremental() const { return mLoader != nullptr; }

    // getDuration():
    // Returns true if there was save with measurable time
    // (and writes it to |duration| if |duration| is not null),
    // otherwise returns false.
    bool getDuration(base::System::Duration* duration) {
        if (mEndTime < mStartTime) {
            return false;
        }

        if (duration) {
            *duration = mEndTime - mStartTime;
        }
        return true;
    }

private:
    struct QueuedPageInfo {
        int blockIndex;
        int32_t pageIndex;  // == pageOffset / block.pageSize
    };

    // The file structure is as follows:
    //
    // 0: 8 bytes, index offset in the file (indexOffset)
    // 8: first nonzero page as struct FileIndex::Page
    // 8 + first page size: second nonzero page
    // ....
    // indexOffset: struct FileIndex
    // EOF

    using Hash = std::array<char, 16>;

    struct FileIndex {
        struct Block {
            RamBlock ramBlock;
            struct Page {
                int32_t sizeOnDisk;  // 0 -> page is all zeroes
                bool same;
                bool hashFilled;
                int64_t filePos;
                Hash hash;

                bool zeroed() const { return sizeOnDisk == 0; }
            };
            std::vector<Page> pages;
        };

        using Flags = IndexFlags;

        int64_t startPosInFile;
        int32_t version = 2;
        int32_t flags = int32_t(Flags::Empty);
        int32_t totalPages = 0;
        std::vector<Block> blocks;

        void clear();
    };

    struct WriteInfo {
        FileIndex::Block::Page* page;
        const uint8_t* ptr;
        bool allocated;
    };

    void calcHash(FileIndex::Block::Page& page,
                  const FileIndex::Block& block,
                  const void* ptr);

    void passToSaveHandler(QueuedPageInfo&& pi);
    bool handlePageSave(QueuedPageInfo&& pi);
    void writeIndex();
    void writePage(WriteInfo&& wi);

    RamLoader* mLoader = nullptr;
    base::StdioStream mStream;
    int mStreamFd;
    Flags mFlags;
    bool mJoined = false;
    bool mHasError = false;
    bool mLoaderOnDemand = false;
    int mLastBlockIndex = -1;
    int64_t mCurrentStreamPos = 8;

    base::Optional<base::ThreadPool<QueuedPageInfo>> mWorkers;
    base::Optional<base::WorkerThread<WriteInfo>> mWriter;

    GapTracker::Ptr mGaps;

    FileIndex mIndex;
    uint64_t mDiskSize = 0;

    static const int kCompressBufferCount = 128;
    using CompressBuffer =
            std::array<uint8_t, compress::maxCompressedSize(kDefaultPageSize)>;
    std::unique_ptr<CompressBuffer[]> mCompressBufferMemory;
    base::Optional<FastReleasePool<CompressBuffer, kCompressBufferCount>>
            mCompressBuffers;

    base::System* mSystem = base::System::get();

    base::System::Duration mStartTime = base::System::get()->getHighResTimeUs();
    base::System::Duration mEndTime = 0;

    IncrementalStats mIncStats;
};

}  // namespace snapshot
}  // namespace android