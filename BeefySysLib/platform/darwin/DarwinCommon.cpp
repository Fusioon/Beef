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

static void* gCoreFoundationLib = NULL;
static void* gCoreServicesLib = NULL;

// libdispatch Function Pointers

static dispatch_queue_t (*bf_dispatch_queue_create)(const char* label, void* attr) = NULL;
static void (*bf_dispatch_async_f)(dispatch_queue_t queue, void* context, dispatch_function_t work) = NULL;
static void (*bf_dispatch_sync_f)(dispatch_queue_t queue, void* context, dispatch_function_t work) = NULL;
static void (*bf_dispatch_release)(void* object) = NULL;

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
static void (*bf_FSEventStreamSetDispatchQueue)(FSEventStreamRef streamRef, dispatch_queue_t q) = NULL;

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
static CFStringRef (*bf_CFStringCreateWithCString)(CFAllocatorRef alloc, const char *cStr, CFStringEncoding encoding) = NULL;
static CFArrayRef (*bf_CFArrayCreate)(CFAllocatorRef allocator, const void **values, CFIndex numValues, const CFArrayCallBacks *callBacks) = NULL;
static void (*bf_CFRelease)(CFTypeRef cf) = NULL;

struct BfpFileWatcher
{
	String					mWatchPath;
	String					mAbsPath;
	BfpDirectoryChangeFunc	mDirectoryChangeFunc;
	FSEventStreamRef		mStreamRef;
	void*					mUserData;
	dispatch_queue_t		mDispatchQueue;

	void ReleaseStream()
	{
		if (mStreamRef == NULL)
			return;

		bf_FSEventStreamStop(mStreamRef);
		bf_FSEventStreamInvalidate(mStreamRef);
		bf_FSEventStreamRelease(mStreamRef);
		mStreamRef = NULL;
	}
};

static bool CheckPathExists(const char* path)
{
	struct stat sb;
	return (lstat(path, &sb) == 0);
}

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

	// We do not handle renames across batches as that would needlessly complicate things
	// if cross batch rename occurs then remove and add is emitted
	Dictionary<uint64_t, String> renamedItems;

	char pathBuffer[PATH_MAX];

	for (CFIndex i = 0; i < numEvents; i++)
	{
		FSEventStreamEventFlags flags = eventFlags[i];

		if (flags & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagUnmount | kFSEventStreamEventFlagMount))
		{
			watcher->mAbsPath.clear();

			char* newAbsPath = realpath(watcher->mWatchPath.c_str(), NULL);
			if (newAbsPath != NULL)
				watcher->mAbsPath = newAbsPath;
			free(newAbsPath);

			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Failed, watcher->mWatchPath.c_str(),NULL, NULL);
			continue;
		}

		if (watcher->mAbsPath.empty())
			continue;

		CFDictionaryRef dict = static_cast<CFDictionaryRef>(bf_CFArrayGetValueAtIndex(dictArray, i));

		CFStringRef pathCF = (CFStringRef)bf_CFDictionaryGetValue(dict, bf_kFSEventStreamEventExtendedDataPathKey);
		if (pathCF == NULL)
			continue;

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
			if (CheckPathExists(absFilePath.c_str()))
				flags &= ~kFSEventStreamEventFlagItemRemoved;
			else
				flags &= ~(kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemCloned);
		}

		if (flags & kFSEventStreamEventFlagItemRenamed)
		{
			CFNumberRef fileIDCF = (CFNumberRef)bf_CFDictionaryGetValue(dict, bf_kFSEventStreamEventExtendedFileIDKey);

			if (fileIDCF == NULL)
				continue;

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
				String* newPath;
				String* oldPath;

				if (CheckPathExists(absFilePath.c_str()))
				{
					newPath = &relativeFilePath;
					oldPath = valPtr;
				}
				else if (CheckPathExists(valPtr->c_str()))
				{
					oldPath = &relativeFilePath;
					newPath = valPtr;
				}
				else
				{
					// Neither path does exist, might be A -> B -> C rename or removed just skip
					continue;
				}

				// Make the stored path relative
				*valPtr =  GetRelativePath(*valPtr, watcher->mAbsPath);

				const auto newDirPathIdx = newPath->LastIndexOf('/');
				const auto oldDirPathIdx = oldPath->LastIndexOf('/');

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
						isRename = (String::Compare(*newPath, 0, *oldPath, 0, newDirPathIdx, false) == 0);
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

				renamedItems.Remove(fileID);
			}

			continue;
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

		if (flags & (
			kFSEventStreamEventFlagItemInodeMetaMod |
			kFSEventStreamEventFlagItemModified |
			kFSEventStreamEventFlagItemXattrMod |
			kFSEventStreamEventFlagItemFinderInfoMod |
			kFSEventStreamEventFlagItemChangeOwner))
		{
			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),relativeFilePath.c_str(), NULL);
		}
	}

	// Moved to/from outside
	for (const auto& kv : renamedItems)
	{
		bool exists = CheckPathExists(kv.mValue.c_str());
		String relativeFilePath = GetRelativePath(kv.mValue, watcher->mAbsPath);
		String relativeDirectoryPath = GetFileDir(relativeFilePath);
		watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, (exists ? BfpFileChangeKind_Added : BfpFileChangeKind_Removed), watcher->mWatchPath.c_str(),relativeFilePath.c_str(), NULL);
		if (!relativeDirectoryPath.empty())
			watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mWatchPath.c_str(),relativeDirectoryPath.c_str(), NULL);
	}

}

template <bool queueDelete>
static void DestroyWatcherOnQueue(void* param)
{
	BfpFileWatcher* watcher = (BfpFileWatcher*)param;
	watcher->ReleaseStream();
	// Has to be queued as there might be events that were enqueued before we released the stream
	// and they will try to read watcher data
	if (queueDelete)
	{
		bf_dispatch_async_f(watcher->mDispatchQueue, watcher, [](void* data) {
			BfpFileWatcher* watcher = (BfpFileWatcher*)data;
			dispatch_queue_t queue = watcher->mDispatchQueue;
			delete watcher;
			bf_dispatch_release(queue);
		});
	}
}

class FsEventFileWatchManager : public FileWatchManager
{
public:
	Array<BfpFileWatcher*>	mWatchers;
	CritSect				mCritSect;
	bool					mInitialized;
public:
	FsEventFileWatchManager()
	{
		mInitialized = false;
	}

    bool Init() override;

    void Shutdown() override;

    BfpFileWatcher* WatchDirectory(const char *path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void *userData, BfpFileResult *outResult) override;

    void Remove(BfpFileWatcher *watcher) override;

};

bool FsEventFileWatchManager::Init()
{
	AutoCrit autoCrit(mCritSect);

	gCoreFoundationLib = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", RTLD_LAZY);
	gCoreServicesLib = dlopen("/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", RTLD_LAZY);
	if ((gCoreFoundationLib == NULL) || (gCoreServicesLib == NULL) )
	{
		return false;
	}

	bool symbolsLoaded = true;

#define BF_DP_GET_SYM(name) (symbolsLoaded &= (bf_##name = (decltype(bf_##name))dlsym(RTLD_DEFAULT, #name)) != NULL)
	BF_DP_GET_SYM(dispatch_queue_create);
	BF_DP_GET_SYM(dispatch_async_f);
	BF_DP_GET_SYM(dispatch_sync_f);
	BF_DP_GET_SYM(dispatch_release);
#undef BF_DP_GET_SYM

#define BF_CF_GET_SYM(name) (symbolsLoaded &= (bf_##name = (decltype(bf_##name))dlsym(gCoreFoundationLib, #name)) != NULL)

	// Resolve CoreFoundation functions
	BF_CF_GET_SYM(CFArrayGetValueAtIndex);
	BF_CF_GET_SYM(CFDictionaryGetValue);
	BF_CF_GET_SYM(CFStringGetCStringPtr);
	BF_CF_GET_SYM(CFStringGetCString);
	BF_CF_GET_SYM(CFNumberGetValue);
	BF_CF_GET_SYM(CFStringCreateWithCString);
	BF_CF_GET_SYM(CFArrayCreate);
	BF_CF_GET_SYM(CFRelease);

#undef BF_CF_GET_SYM


#define BF_CS_GET_SYM(name) (symbolsLoaded &= (bf_##name = (decltype(bf_##name))dlsym(gCoreServicesLib, #name)) != NULL)

	// Resolve CoreServices functions
	BF_CS_GET_SYM(FSEventStreamCreate);
	BF_CS_GET_SYM(FSEventStreamSetDispatchQueue);
	BF_CS_GET_SYM(FSEventStreamStart);
	BF_CS_GET_SYM(FSEventStreamStop);
	BF_CS_GET_SYM(FSEventStreamInvalidate);
	BF_CS_GET_SYM(FSEventStreamRelease);

#undef BF_CS_GET_SYM

	if (!symbolsLoaded)
	{
		return false;
	}

	// Create constants, these are not exported so cannot resolve from library

#define BF_CREATE_CONST_STR(name, value) (symbolsLoaded &= (bf_##name = bf_CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8)) != NULL)

	BF_CREATE_CONST_STR(kFSEventStreamEventExtendedDataPathKey, "path");
	BF_CREATE_CONST_STR(kFSEventStreamEventExtendedFileIDKey, "fileID");

#undef BF_CREATE_CONST_STR

	if (!symbolsLoaded)
		return false;

	mInitialized = true;
	return true;
}

void FsEventFileWatchManager::Shutdown()
{
	Array<BfpFileWatcher*> watchersToRelease;
	{
		AutoCrit autoCrit(mCritSect);
		if (!mInitialized)
			return;

		mInitialized = false;

		watchersToRelease = std::move(mWatchers);
	}

	for (const auto& watcher : watchersToRelease)
		bf_dispatch_async_f(watcher->mDispatchQueue, watcher, &DestroyWatcherOnQueue<false>);

	for (const auto& watcher : watchersToRelease)
	{
		// Drain barrier: on a serial queue this returns only once every block has completed
		bf_dispatch_sync_f(watcher->mDispatchQueue, NULL, [](void*) {});
		bf_dispatch_release(watcher->mDispatchQueue);
		delete watcher;
	}

	// Leaking constants / dlopen symbols is intentional
}

BfpFileWatcher* FsEventFileWatchManager::WatchDirectory(const char* path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void* userData, BfpFileResult* outResult)
{
	AutoCrit autoCrit(mCritSect);

	if (!mInitialized)
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
	defer ( free(absPath) );

	CFStringRef pathString = bf_CFStringCreateWithCString(NULL, absPath, kCFStringEncodingUTF8);
	if (pathString == NULL)
	{
		OUTRESULT(BfpFileResult_UnknownError);
		return NULL;
	}
	defer( bf_CFRelease(pathString) );

    CFArrayRef pathsToWatch = bf_CFArrayCreate(NULL, (const void **)&pathString, 1, NULL);
	if (pathsToWatch == NULL)
	{
		OUTRESULT(BfpFileResult_UnknownError);
		return NULL;
	}
	defer( bf_CFRelease(pathsToWatch) );

	BfpFileWatcher* pFileWatcher = new BfpFileWatcher();
	pFileWatcher->mWatchPath = path;
	pFileWatcher->mAbsPath = absPath;
	pFileWatcher->mDirectoryChangeFunc = callback;
	pFileWatcher->mUserData = userData;

    FSEventStreamContext context = { };
	context.info = pFileWatcher;
    pFileWatcher->mStreamRef = bf_FSEventStreamCreate(
        NULL,
        ((flags & BfpFileWatcherFlag_IncludeSubdirectories) != 0 ? &FSEventsCallback<false> : &FSEventsCallback<true>),
        &context,
        pathsToWatch,
        kFSEventStreamEventIdSinceNow,
        0.1,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagUseExtendedData | kFSEventStreamCreateFlagUseCFTypes
    );

	if (pFileWatcher->mStreamRef == NULL)
	{
		delete pFileWatcher;
		OUTRESULT(BfpFileResult_UnknownError);
		return NULL;
	}

	pFileWatcher->mDispatchQueue = bf_dispatch_queue_create("com.beef.filewatcher", NULL);
	if (pFileWatcher->mDispatchQueue == NULL)
	{
    	bf_FSEventStreamRelease(pFileWatcher->mStreamRef);
		delete pFileWatcher;
		OUTRESULT(BfpFileResult_UnknownError);
		return NULL;
	}

	bf_FSEventStreamSetDispatchQueue(pFileWatcher->mStreamRef, pFileWatcher->mDispatchQueue);
    if (!bf_FSEventStreamStart(pFileWatcher->mStreamRef))
    {
    	bf_FSEventStreamInvalidate(pFileWatcher->mStreamRef);
    	bf_FSEventStreamRelease(pFileWatcher->mStreamRef);
    	bf_dispatch_release(pFileWatcher->mDispatchQueue);
	    delete pFileWatcher;
    	OUTRESULT(BfpFileResult_UnknownError);
    	return NULL;
    }

	mWatchers.Add(pFileWatcher);
	OUTRESULT(BfpFileResult_Ok);
	return pFileWatcher;
}

void FsEventFileWatchManager::Remove(BfpFileWatcher* watcher)
{

	bool removed;
	{
		AutoCrit autoCrit(mCritSect);
		removed = mWatchers.Remove(watcher);
	}

	// Events might still be generated for a short while
	// in this case it's not a problem since beef side has it's own
	// list of registered watchers
	if (removed)
	{
		bf_dispatch_async_f(watcher->mDispatchQueue, watcher, &DestroyWatcherOnQueue<true>);
	}
}

FileWatchManager* FileWatchManager::Allocate()
{
    return new FsEventFileWatchManager();
}

#endif // BFP_HAS_FILEWATCHER
