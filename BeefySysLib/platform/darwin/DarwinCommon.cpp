#include <execinfo.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

#define lseek64 lseek
#define ftruncate64 ftruncate

#define BFP_HAS_FILEWATCHER

#include "../posix/PosixCommon.cpp"

char* itoa(int value, char* str, int base)
{
    if (base == 16)
        sprintf(str, "%X", value);
    else
        sprintf(str, "%d", value);
    return str;
}

#ifdef BFP_HAS_FILEWATCHER

#include <CoreServices/CoreServices.h>
#include <atomic>

// FSEvents Function Pointers
static FSEventStreamRef (*bf_FSEventStreamCreate)(
	CFAllocatorRef allocator,
	FSEventStreamCallback callback,
	FSEventStreamContext *context,
	CFArrayRef pathsToWatch,
	FSEventStreamEventId sinceWhen,
	CFTimeInterval latency,
	FSEventStreamCreateFlags flags
) = NULL;

static void (*bf_FSEventStreamScheduleWithRunLoop)(
	FSEventStreamRef streamRef,
	CFRunLoopRef runLoop,
	CFStringRef runLoopMode
) = NULL;

static Boolean (*bf_FSEventStreamStart)(FSEventStreamRef streamRef) = NULL;
static void (*bf_FSEventStreamStop)(FSEventStreamRef streamRef) = NULL;
static void (*bf_FSEventStreamInvalidate)(FSEventStreamRef streamRef) = NULL;
static void (*bf_FSEventStreamRelease)(FSEventStreamRef streamRef) = NULL;

// FSEvents Constant
static CFStringRef bf_kFSEventStreamEventExtendedDataPathKey = NULL;
static CFStringRef bf_kFSEventStreamEventExtendedFileIDKey = NULL;

// CoreFoundation Function Pointers
static const void* (*bf_CFArrayGetValueAtIndex)(CFArrayRef theArray, CFIndex idx) = NULL;
static const void* (*bf_CFDictionaryGetValue)(CFDictionaryRef theDict, const void *key) = NULL;
static const char* (*bf_CFStringGetCStringPtr)(CFStringRef theString, CFStringEncoding encoding) = NULL;
static Boolean (*bf_CFStringGetCString)(CFStringRef theString, char *buffer, CFIndex bufferSize, CFStringEncoding encoding) = NULL;
static Boolean (*bf_CFNumberGetValue)(CFNumberRef number, CFNumberType theType, void *valuePtr) = NULL;
static CFRunLoopRef (*bf_CFRunLoopGetCurrent)(void) = NULL;
static void (*bf_CFRunLoopRun)(void) = NULL;
static void (*bf_CFRunLoopStop)(CFRunLoopRef rl) = NULL;
static CFStringRef (*bf_CFStringCreateWithCString)(CFAllocatorRef alloc, const char *cStr, CFStringEncoding encoding) = NULL;
static CFArrayRef (*bf_CFArrayCreate)(CFAllocatorRef allocator, const void **values, CFIndex numValues, const CFArrayCallBacks *callBacks) = NULL;
static void (*bf_CFRunLoopWakeUp)(CFRunLoopRef rl) = NULL;
static void (*bf_CFRelease)(CFTypeRef cf) = NULL;
static CFRunLoopSourceRef (*bf_CFRunLoopSourceCreate)(CFAllocatorRef allocator, CFIndex order, CFRunLoopSourceContext *context) = NULL;
static void (*bf_CFRunLoopAddSource)(CFRunLoopRef rl, CFRunLoopSourceRef source, CFRunLoopMode mode) = NULL;
static void (*bf_CFRunLoopRemoveSource)(CFRunLoopRef rl, CFRunLoopSourceRef source, CFRunLoopMode mode) = NULL;
static void (*bf_CFRunLoopSourceInvalidate)(CFRunLoopSourceRef source) = NULL;
static void (*bf_CFRunLoopSourceSignal)(CFRunLoopSourceRef source) = NULL;

// CoreFoundation Exported Constants
static CFAllocatorRef* bf_kCFAllocatorDefault = NULL;
static CFStringRef* bf_kCFRunLoopDefaultMode = NULL;

struct BfpFileWatcher
{
	String mWatchPath;
	String mAbsPath;
	BfpDirectoryChangeFunc mDirectoryChangeFunc;
	FSEventStreamRef mStreamRef;
	void* mUserData;
};


// Callback for watcher
template <bool filter>
static void FSEventsCallback(
    ConstFSEventStreamRef streamRef,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventData,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[])
{
	BfpFileWatcher* watcher = (BfpFileWatcher*)clientCallBackInfo;
	CFArrayRef dictArray = static_cast<CFArrayRef>(eventData);

	Dictionary<uint64_t, String> renamedItems;
	HashSet<uint64_t> handledRenameIds;

	char pathBuffer[PATH_MAX];

	for (CFIndex i = 0; i < numEvents; i++)
	{
		FSEventStreamEventFlags flags = eventFlags[i];

		if (flags & kFSEventStreamEventFlagRootChanged)
		{
			watcher->mAbsPath.clear();

			char* newAbsPath = realpath(watcher->mWatchPath.c_str(), NULL);
			if (newAbsPath != NULL)
				watcher->mAbsPath = newAbsPath;
			free(newAbsPath);
			continue;
		}

		if (watcher->mAbsPath.empty())
			continue;

		CFDictionaryRef dict = static_cast<CFDictionaryRef>(bf_CFArrayGetValueAtIndex(dictArray, i));

		CFStringRef pathCF = static_cast<CFStringRef>(
			bf_CFDictionaryGetValue(dict, bf_kFSEventStreamEventExtendedDataPathKey)
		);

		const char* path = bf_CFStringGetCStringPtr(pathCF, kCFStringEncodingUTF8);
		if (path == NULL)
		{
			if (!bf_CFStringGetCString(pathCF, pathBuffer, sizeof(pathBuffer), kCFStringEncodingUTF8))
				continue;

			path = pathBuffer;
		}

		// Make event path relative to watched path
		String absFilePath = StringImpl::MakeRef(path);
		String relativeFilePath = GetRelativePath(absFilePath, watcher->mAbsPath);
		String relativeDirectoryPath = GetFileDir(relativeFilePath);

		// Check if event happened inside subdirectory and should be ignored
		// - when registered without BfpFileWatcherFlag_IncludeSubdirectories
		if ((filter) && (!relativeDirectoryPath.empty()))
		{
			// Trigger modified/error for containing directory

			if (flags & (kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagMustScanSubDirs))
			{
				if (relativeDirectoryPath.IndexOf('/') == -1)
				{
					watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Failed, watcher->mWatchPath.c_str(),relativeDirectoryPath.c_str(), NULL);
				}
			}
			else if (flags & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemCloned | kFSEventStreamEventFlagItemRemoved))
			{
				if (relativeDirectoryPath.IndexOf('/') == -1)
				{
					watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),relativeDirectoryPath.c_str(), NULL);
				}
			}

			continue;
		}

		if (flags & (kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagMustScanSubDirs))
		{
			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Failed, watcher->mWatchPath.c_str(),relativeFilePath.c_str(), NULL);
			continue;
		}

		// Since events are coalesced into single event, we can have created and removed set at the same time
		// Check if the file exists and remove the invalid flag
		if ((flags & kFSEventStreamEventFlagItemRemoved) && (flags & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemCloned)))
		{
			struct stat sb;
			bool exists = (stat(absFilePath.c_str(), &sb) == 0);
			if (exists)
				flags &= ~kFSEventStreamEventFlagItemRemoved;
			else
				flags &= ~(kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemCloned);
		}

		if (flags & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemCloned))
		{
			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Added, watcher->mWatchPath.c_str(),relativeFilePath.c_str(), NULL);
			if (!relativeDirectoryPath.empty())
				watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),relativeDirectoryPath.c_str(), NULL);
		}
		else if (flags & kFSEventStreamEventFlagItemRemoved)
		{
			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Removed, watcher->mWatchPath.c_str(),relativeFilePath.c_str(), NULL);
			if (!relativeDirectoryPath.empty())
				watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),relativeDirectoryPath.c_str(), NULL);
		}
		else if (flags & (
			kFSEventStreamEventFlagItemInodeMetaMod |
			kFSEventStreamEventFlagItemModified |
			kFSEventStreamEventFlagItemXattrMod |
			kFSEventStreamEventFlagItemFinderInfoMod |
			kFSEventStreamEventFlagItemChangeOwner))
		{
			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),relativeFilePath.c_str(), NULL);
		}

		if (flags & kFSEventStreamEventFlagItemRenamed)
		{
			CFNumberRef fileIDCF = static_cast<CFNumberRef>(
				bf_CFDictionaryGetValue(dict, bf_kFSEventStreamEventExtendedFileIDKey)
			);
			uint64_t fileID = 0;
			bf_CFNumberGetValue(fileIDCF, kCFNumberLongLongType, &fileID);

			uint64_t* keyPtr;
			String* valPtr;

			if (renamedItems.TryAdd(fileID, &keyPtr, &valPtr))
			{
				*valPtr = absFilePath;
			}
			else
			{
				struct stat sb;
				bool currentFileExists = (stat(absFilePath.c_str(), &sb) == 0);
				bool storedFileExists = (stat(valPtr->c_str(), &sb) == 0);

				if (currentFileExists)
				{
					
				}

				// Make the stored path relative
				String oldPath = GetRelativePath(*valPtr, watcher->mAbsPath);

				const auto newDirPathIdx = relativeFilePath.LastIndexOf('/');
				const auto oldDirPathIdx = oldPath.LastIndexOf('/');

				// Only handle as rename if it is within the same directory
				bool isRename = false;
				if (newDirPathIdx == oldDirPathIdx)
				{
					if (newDirPathIdx == -1)
					{
						isRename = true;
					}
					else
					{
						isRename = (String::Compare(relativeFilePath, 0, oldPath, 0, newDirPathIdx, false) == 0);
					}
				}

				if (isRename)
				{
					watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Renamed, watcher->mWatchPath.c_str(),newPath->c_str(), oldPath->c_str());
				}
				else
				{
					watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Removed, watcher->mWatchPath.c_str(),oldPath->c_str(), NULL);
					watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Added, watcher->mWatchPath.c_str(),newPath->c_str(), NULL);

					// trigger modified on directories where the changes happened
					String dir = GetFileDir(*oldPath);
					if (!dir.empty())
						watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),dir.c_str(), NULL);
					dir = GetFileDir(*newPath);
					if (!dir.empty())
						watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),dir.c_str(), NULL);
				}

				// There might be a case where there are odd number of renames A -> B -> C
				// so don't delete just update the entry and mark as handled
				*valPtr = absFilePath;
				handledRenameIds.Add(fileID);
			}
		}
	}

	// Moved to/from outside
	for (const auto& kv : renamedItems)
	{
		struct stat buffer;
		bool exists = (stat(kv.mValue.mPath.c_str(), &buffer) == 0);
		String relativeFilePath = GetRelativePath(kv.mValue.mPath, watcher->mAbsPath);
		String relativeDirectoryPath = GetFileDir(relativeFilePath);
		watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, (exists ? BfpFileChangeKind_Added : BfpFileChangeKind_Removed), watcher->mWatchPath.c_str(),relativeFilePath.c_str(), NULL);
		if (!relativeDirectoryPath.empty())
			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),relativeDirectoryPath.c_str(), NULL);
	}

}

class FsEventFileWatchManager : public FileWatchManager
{
public:
	void* mCoreFoundationLib;
	void* mCoreServicesLib;
	BfpThread* mWatcherThread;
	CFRunLoopRef mRunLoopRef;
	CritSect mCritSect;
	SyncEvent mWatcherReadyEvent;
	std::atomic<bool> mShouldExit;

public:
	FsEventFileWatchManager() :
		mCoreFoundationLib(NULL),
		mCoreServicesLib(NULL),
		mWatcherThread(NULL),
		mRunLoopRef(NULL),
		mWatcherReadyEvent(true, false),
		mShouldExit(true)
	{
	}

    bool Init() override;

    void Shutdown() override;

    BfpFileWatcher* WatchDirectory(const char *path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void *userData, BfpFileResult *outResult) override;

    void Remove(BfpFileWatcher *watcher) override;

    void WorkerThreadProc();
};

static void keepalive_perform(void*) {}

void FsEventFileWatchManager::WorkerThreadProc()
{
	mRunLoopRef = bf_CFRunLoopGetCurrent();

	// Create dummy source so CFRunLoopRun does not exit if there is are no file watchers active
	CFRunLoopSourceContext ctx = { };
	ctx.perform = keepalive_perform;
	CFRunLoopSourceRef keepAliveSource = bf_CFRunLoopSourceCreate(*bf_kCFAllocatorDefault, 0, &ctx);
	bf_CFRunLoopAddSource(mRunLoopRef, keepAliveSource, *bf_kCFRunLoopDefaultMode);
	mWatcherReadyEvent.Set();

	while (!mShouldExit.load())
	{
		bf_CFRunLoopRun();
	}

	bf_CFRunLoopRemoveSource(mRunLoopRef, keepAliveSource, *bf_kCFRunLoopDefaultMode);
	bf_CFRunLoopSourceInvalidate(keepAliveSource);
	bf_CFRelease(keepAliveSource);

	{
		AutoCrit critsect(mCritSect);
		mRunLoopRef = NULL;
	}

	// drain anything enqueued before that
	// for (int i = 100; i > 0; i--)
	// {
	// 	if (bf_CFRunLoopRunInMode(*bf_kCFRunLoopDefaultMode, 0, false) != kCFRunLoopRunFinished)
	// 		break;
	// }
}

static void WatcherWorkThreadThunk(void* userData)
{
    ((FsEventFileWatchManager*)userData)->WorkerThreadProc();
}

bool FsEventFileWatchManager::Init()
{
	AutoCrit critsect(mCritSect);

	mCoreFoundationLib = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", RTLD_LAZY);
	mCoreServicesLib = dlopen("/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", RTLD_LAZY);
	if ((mCoreFoundationLib == NULL) || (mCoreServicesLib == NULL) )
	{
		// Set event so we don't lock in WatchDirectory
		mWatcherReadyEvent.Set();
		return false;
	}

	bool symbolsLoaded = true;

#define BF_CF_GET_SYM(name) (symbolsLoaded &= (bf_##name = (decltype(bf_##name))dlsym(mCoreFoundationLib, #name)) != NULL)

	// Resolve CoreFoundation functions
	BF_CF_GET_SYM(CFArrayGetValueAtIndex);
	BF_CF_GET_SYM(CFDictionaryGetValue);
	BF_CF_GET_SYM(CFStringGetCStringPtr);
	BF_CF_GET_SYM(CFStringGetCString);
	BF_CF_GET_SYM(CFNumberGetValue);
	BF_CF_GET_SYM(CFRunLoopGetCurrent);
	BF_CF_GET_SYM(CFRunLoopRun);
	BF_CF_GET_SYM(CFRunLoopStop);
	BF_CF_GET_SYM(CFStringCreateWithCString);
	BF_CF_GET_SYM(CFArrayCreate);
	BF_CF_GET_SYM(CFRunLoopWakeUp);
	BF_CF_GET_SYM(CFRelease);
	BF_CF_GET_SYM(CFRunLoopSourceCreate);
	BF_CF_GET_SYM(CFRunLoopAddSource);
	BF_CF_GET_SYM(CFRunLoopRemoveSource);
	BF_CF_GET_SYM(CFRunLoopSourceInvalidate);
	BF_CF_GET_SYM(CFRunLoopSourceSignal);

	// Resolve CoreFoundation exported constants
	BF_CF_GET_SYM(kCFAllocatorDefault);
	BF_CF_GET_SYM(kCFRunLoopDefaultMode);

#undef BF_CF_GET_SYM


#define BF_CS_GET_SYM(name) (symbolsLoaded &= (bf_##name = (decltype(bf_##name))dlsym(mCoreServicesLib, #name)) != NULL)

	// Resolve CoreServices functions
	BF_CS_GET_SYM(FSEventStreamCreate);
	BF_CS_GET_SYM(FSEventStreamScheduleWithRunLoop);
	BF_CS_GET_SYM(FSEventStreamStart);
	BF_CS_GET_SYM(FSEventStreamStop);
	BF_CS_GET_SYM(FSEventStreamInvalidate);
	BF_CS_GET_SYM(FSEventStreamRelease);

#undef BF_CS_GET_SYM

	if (!symbolsLoaded)
	{
		mWatcherReadyEvent.Set();
		return false;
	}

	// Create constants, cannot resolve from library

	bf_kFSEventStreamEventExtendedDataPathKey = bf_CFStringCreateWithCString(*bf_kCFAllocatorDefault, "path", kCFStringEncodingUTF8);
	bf_kFSEventStreamEventExtendedFileIDKey = bf_CFStringCreateWithCString(*bf_kCFAllocatorDefault, "fileID", kCFStringEncodingUTF8);

	mShouldExit.store(false);
    mWatcherThread = BfpThread_Create(&WatcherWorkThreadThunk, this);
    if (mWatcherThread == NULL)
    {
    	mWatcherReadyEvent.Set();
	    return false;
    }

	return true;
}

void FsEventFileWatchManager::Shutdown()
{
	{
		AutoCrit critsect(mCritSect);

		mShouldExit.store(true);
		if (mRunLoopRef != NULL)
		{
			bf_CFRunLoopStop(mRunLoopRef);
		}
	}


	bool doUnload = true;
	if (mWatcherThread != NULL)
	{
		doUnload = BfpThread_WaitFor(mWatcherThread, 1000);
		BfpThread_Release(mWatcherThread);
		mWatcherThread = NULL;
	}

	if (doUnload)
	{
		if (bf_CFRelease)
		{
			if (bf_kFSEventStreamEventExtendedDataPathKey)
					bf_CFRelease(bf_kFSEventStreamEventExtendedDataPathKey);
			bf_kFSEventStreamEventExtendedDataPathKey = NULL;

			if (bf_kFSEventStreamEventExtendedFileIDKey)
				bf_CFRelease(bf_kFSEventStreamEventExtendedFileIDKey);
			bf_kFSEventStreamEventExtendedFileIDKey = NULL;
		}

		if (mCoreFoundationLib != NULL)
			dlclose(mCoreFoundationLib);

		mCoreFoundationLib = NULL;

		if (mCoreServicesLib)
			dlclose(mCoreServicesLib);

		mCoreServicesLib = NULL;
	}
}

BfpFileWatcher* FsEventFileWatchManager::WatchDirectory(const char* path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void* userData, BfpFileResult* outResult)
{
	mWatcherReadyEvent.WaitFor();

	AutoCrit critsect(mCritSect);
	if (mRunLoopRef == NULL)
	{
		OUTRESULT(BfpFileResult_UnknownError);
		return NULL;
	}

	char* absPath = realpath(path, NULL);
	if (absPath == NULL)
	{
		OUTRESULT(BfpFileResult_NotFound);
		return NULL;
	}

	CFStringRef mypath = bf_CFStringCreateWithCString(*bf_kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = bf_CFArrayCreate(NULL, (const void **)&mypath, 1, NULL);
	defer(
		bf_CFRelease(mypath);
		bf_CFRelease(pathsToWatch);
	);

	BfpFileWatcher* pFileWatcher = new BfpFileWatcher();
	pFileWatcher->mWatchPath = path;
	pFileWatcher->mAbsPath = absPath;
	free(absPath);
	pFileWatcher->mDirectoryChangeFunc = callback;
	pFileWatcher->mUserData = userData;

    FSEventStreamContext context = { };
	context.info = pFileWatcher;
    pFileWatcher->mStreamRef = bf_FSEventStreamCreate(
        *bf_kCFAllocatorDefault,
        ((flags & BfpFileWatcherFlag_IncludeSubdirectories) != 0 ? &FSEventsCallback<false> : &FSEventsCallback<true>),
        &context,
        pathsToWatch,
        kFSEventStreamEventIdSinceNow,
        0.1,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagUseExtendedData | kFSEventStreamCreateFlagUseCFTypes
    );

    // Attach the new stream to the BACKGROUND thread's Run Loop
    bf_FSEventStreamScheduleWithRunLoop(pFileWatcher->mStreamRef, mRunLoopRef, *bf_kCFRunLoopDefaultMode);
    if (!bf_FSEventStreamStart(pFileWatcher->mStreamRef))
    {
    	bf_FSEventStreamInvalidate(pFileWatcher->mStreamRef);
    	bf_FSEventStreamRelease(pFileWatcher->mStreamRef);
	    delete pFileWatcher;
    	OUTRESULT(BfpFileResult_UnknownError);
    	return NULL;
    }
    bf_CFRunLoopWakeUp(mRunLoopRef);

	OUTRESULT(BfpFileResult_Ok);
	return pFileWatcher;
}

struct BfpFileWatcherRemoveInfo
{
	SyncEvent mWatcherDeletedEvent;
	BfpFileWatcher* mFileWatcher;
	CFRunLoopSourceRef mSource;

	BfpFileWatcherRemoveInfo() :
		mWatcherDeletedEvent(true, false),
		mFileWatcher(NULL),
		mSource(NULL)
	{

	}
};

static void WatcherRemovePerform(void* context)
{
	BfpFileWatcherRemoveInfo* info = (BfpFileWatcherRemoveInfo*)context;

	bf_FSEventStreamStop(info->mFileWatcher->mStreamRef);
	bf_FSEventStreamInvalidate(info->mFileWatcher->mStreamRef);
	bf_FSEventStreamRelease(info->mFileWatcher->mStreamRef);
	delete info->mFileWatcher;
	bf_CFRunLoopSourceInvalidate(info->mSource);
	info->mWatcherDeletedEvent.Set();
}

void FsEventFileWatchManager::Remove(BfpFileWatcher* watcher)
{
	AutoCrit critsect(mCritSect);

	if (mShouldExit.load())
	{
		bf_FSEventStreamStop(watcher->mStreamRef);
		bf_FSEventStreamInvalidate(watcher->mStreamRef);
		bf_FSEventStreamRelease(watcher->mStreamRef);
		delete watcher;
	}
	else
	{
		BfpFileWatcherRemoveInfo removeInfo;
		removeInfo.mFileWatcher = watcher;

		CFRunLoopSourceContext removeCtx = { };
		removeCtx.info = &removeInfo;
		removeCtx.perform = WatcherRemovePerform;

		removeInfo.mSource = bf_CFRunLoopSourceCreate(*bf_kCFAllocatorDefault, 0, &removeCtx);

		bf_CFRunLoopAddSource(mRunLoopRef, removeInfo.mSource, *bf_kCFRunLoopDefaultMode);
		bf_CFRunLoopSourceSignal(removeInfo.mSource);
		bf_CFRunLoopWakeUp(mRunLoopRef);

		removeInfo.mWatcherDeletedEvent.WaitFor();
		bf_CFRelease(removeInfo.mSource);
	}
}

FileWatchManager* FileWatchManager::Allocate()
{
    return new FsEventFileWatchManager();
}

#endif // BFP_HAS_FILEWATCHER
