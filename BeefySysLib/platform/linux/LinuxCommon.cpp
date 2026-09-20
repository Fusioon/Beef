#define BFP_HAS_EXECINFO
#define BFP_HAS_PTHREAD_TIMEDJOIN_NP
#define BFP_HAS_PTHREAD_GETATTR_NP
#define BFP_HAS_DLINFO
#define BFP_HAS_FILEWATCHER

#include "../posix/PosixCommon.cpp"

#ifdef BFP_HAS_FILEWATCHER

#include <sys/inotify.h>

#if 0
#define WATCHER_ERRPRINTF(...) BFP_ERRPRINTF(__VA_ARGS__)
#else
#define WATCHER_ERRPRINTF(...) ((void)0)
#endif

struct WatchHandle
{
	int32 mIdx;

	static constexpr int32 INVALID_INDEX = -1;

	static constexpr WatchHandle Invalid() { return { INVALID_INDEX }; }

	bool IsValid() const { return (mIdx != INVALID_INDEX); }
};

struct BfpFileWatcher
{
    String mPath;
    BfpDirectoryChangeFunc mDirectoryChangeFunc;
    WatchHandle mHandle;
    BfpFileWatcherFlags mFlags;
    void* mUserData;

	bool mMarkedForDeletion;
};

class InotifyWorkerThread
{
public:
	struct Slot
	{
		int		mWd = -1;
		int		mParent = -1;
		int		mFirstChild = -1;
		int		mNextSibling = -1;
		uint32	mHandleSubdirs = 0;
		String	mName;
		Array<BfpFileWatcher*> mWatchers;

		bool HandleSubdirs() const { return mHandleSubdirs > 0; }

		void CopyRecursiveWatchersFromSlot(Slot* slot)
		{
			for (auto watcher : slot->mWatchers)
			{
				if ((watcher->mFlags & BfpFileWatcherFlag_IncludeSubdirectories) == 0)
					continue;

				mWatchers.Add(watcher);
				mHandleSubdirs++;
			}
		}

		void AddWatcher(BfpFileWatcher* watcher)
		{
			mWatchers.Add(watcher);
			if (watcher->mFlags & BfpFileWatcherFlag_IncludeSubdirectories)
				mHandleSubdirs++;
		}

		void RemoveWatcher(BfpFileWatcher* watcher)
		{
			mWatchers.Remove(watcher);
			if (watcher->mFlags & BfpFileWatcherFlag_IncludeSubdirectories)
				mHandleSubdirs--;
		}

		void SendEvents(InotifyWorkerThread* manager, const inotify_event* event, BfpFileChangeKind kind, const inotify_event* pairedEvent = NULL, Slot* pairedEventSlot = NULL)
		{
			for (BfpFileWatcher* watcher : mWatchers)
			{
				switch (kind)
				{
					case BfpFileChangeKind_Added:
					case BfpFileChangeKind_Removed:
					case BfpFileChangeKind_Modified:
					{
						String path = manager->GetRelativePath(this, watcher->mHandle, event->name);
						watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, kind, watcher->mPath.c_str(), path.c_str(), NULL);
					}
					break;

					case BfpFileChangeKind_Failed:
					{
						watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, kind, watcher->mPath.c_str(), NULL, NULL);
					}
					break;

					case BfpFileChangeKind_Renamed:
					{
						// Only emit rename if the move happened inside the same directory
						if (this == pairedEventSlot)
						{
							String path = manager->GetRelativePath(this, watcher->mHandle, {});
							String oldPath = path + pairedEvent->name;
							String newPath = path + event->name;
							watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, kind, watcher->mPath.c_str(), oldPath.c_str(), newPath.c_str());
						}
						else
						{
							String path = manager->GetRelativePath(pairedEventSlot, watcher->mHandle, pairedEvent->name);
							watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Removed, watcher->mPath.c_str(), path.c_str(), NULL);
							path = manager->GetRelativePath(this, watcher->mHandle, event->name);
							watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Removed, watcher->mPath.c_str(), path.c_str(), NULL);
						}
					}
					break;
				}
			}
		}
	};

	static constexpr size_t MIN_INOTIFY_EVENT_COUNT = 32;
	static constexpr size_t MAX_INOTIFY_EVENT_SIZE = (sizeof(inotify_event) + PATH_MAX + 1);
	static constexpr size_t NOTIFY_BUFFER_SIZE = (MIN_INOTIFY_EVENT_COUNT * MAX_INOTIFY_EVENT_SIZE);

	int mInotifyHandle = -1;
	int mShutdownPipe[2] = { -1, -1 };

	volatile bool mShuttingDown = false;
	pthread_t mWorkerThread = NULL;
	Dictionary<int, Slot> mWatchSlots;

	CritSect mCritSect;
	Array<BfpFileWatcher*> mDeferredWatchers;
	alignas(inotify_event) char mEventBuffer[NOTIFY_BUFFER_SIZE];

public:
	void WorkerProc();
	bool Init();
	void Shutdown();

	WatchHandle AddWatchPath(const String& path)
	{
		return InotifyWatchPath(path, NULL);
	}

	void AddWatcher(BfpFileWatcher* watcher)
	{
		AutoCrit autoCrit(mCritSect);
		mDeferredWatchers.Add(watcher);
	}

	void DeleteWatcher(BfpFileWatcher* watcher)
	{
		AutoCrit autoCrit(mCritSect);
		watcher->mMarkedForDeletion = true;
		mDeferredWatchers.Add(watcher);
	}

private:
	String GetRelativePath(const Slot* slot, WatchHandle parent, const StringView& path);

	String GetRelativePath(const Slot* slot, int parent, const StringView& path);

	void DispatchEvent(const inotify_event* event, Array<const inotify_event*> unhandledEvents);

	void DispatchUnhandledEvent(const inotify_event* event);

    void HandleDirAdd(const inotify_event* event, Slot* slot, const StringView& path, bool moved)
    {
    	if ((event->mask & IN_ISDIR) == 0)
    		return;

        if (!slot->mHandleSubdirs)
            return;

		if (slot->mWatchers.IsEmpty())
			return;

    	auto watcher = slot->mWatchers[0];

		auto relPath = GetRelativePath(slot, watcher->mHandle, path);
		auto fullPath = watcher->mPath + relPath;

    	Slot* addedSlot;
        auto watchHandle = InotifyWatchPath(fullPath, &addedSlot);
        if (watchHandle.IsValid())
        {
            WATCHER_ERRPRINTF("Failed to add watch for subdirectory '%s' (%d)\n", dirPath.c_str(), errno);
            return;
        }

        WatchSubdirectories(relPath.c_str(), slot, !moved, event->wd);
    }

    WatchHandle InotifyWatchPath(const String& path, Slot** resultSlot);

	void InotifyRemoveDirectoryWatcher(BfpFileWatcher* fileWatch);

	void WatchSubdirectories(const String& path, Slot* slot, bool sendEvents, int parentWd);
};

class InotifyFileWatchManager : public FileWatchManager
{
	SyncEvent mInitializedEvent;
	InotifyWorkerThread mWorkerThread;

public:

	InotifyFileWatchManager() :
		mInitializedEvent(true, false)
	{

	}

	bool Init() override;

	void Shutdown() override;

	BfpFileWatcher* WatchDirectory(const char* path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void* userData, BfpFileResult* outResult) override;

	void Remove(BfpFileWatcher* watcher) override;
};

void InotifyWorkerThread::WorkerProc()
{
	struct pollfd fds[2];
	fds[0].fd = mInotifyHandle;
	fds[0].events = POLLIN;
	fds[1].fd = mShutdownPipe[0];
	fds[1].events = POLLIN;

	Array<const inotify_event*> unhandledEvents;
	while (!mShuttingDown)
	{
		fds[0].revents = 0;
		fds[1].revents = 0;

		int pollResult = poll(fds, 2, -1);
		if (pollResult < 0)
		{
			if (errno == EINTR)
				continue;
			WATCHER_ERRPRINTF("inotify poll failed (%d)\n", errno);
			break;
		}

		// Shutdown was signalled via the self-pipe.
		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
			break;

		if (!(fds[0].revents & POLLIN))
			continue;

		ssize_t length = read(mInotifyHandle, &mEventBuffer, NOTIFY_BUFFER_SIZE);
		if (length < 0)
		{
			switch (errno)
			{
				case EINTR:
				case EAGAIN:
					continue;

				default:
					break;
			}

			WATCHER_ERRPRINTF("Failed to read inotify event data!\n");
			return;
		}

		ssize_t pos = 0;
		while(pos < length)
		{
			inotify_event* event = (inotify_event*) &mEventBuffer[pos];
			DispatchEvent(event, unhandledEvents);
			pos += sizeof(inotify_event) + event->len;
		}

		for (auto event : unhandledEvents)
		{
			DispatchUnhandledEvent(event);
		}
		unhandledEvents.Clear();

		AutoCrit autoCrit(mCritSect);
		for (auto watcher : mDeferredWatchers)
		{
			if (watcher->mMarkedForDeletion)
			{
				delete watcher;
				return;
			}
			else
			{
				int* keyPtr;
				Slot* valPtr;
				mWatchSlots.TryAdd(watcher->mHandle.mIdx, &keyPtr, &valPtr);
				valPtr->mWd = watcher->mHandle.mIdx;
				valPtr->AddWatcher(watcher);
			}

		}
		mDeferredWatchers.Clear();
	}
}

String InotifyWorkerThread::GetRelativePath(const Slot *slot, WatchHandle parent, const StringView &path)
{
	Slot* parentSlot;
	if (!mWatchSlots.TryGetValue(parent.mIdx, &parentSlot))
		return {};

	return GetRelativePath(slot, parentSlot->mWd, path);
}

String InotifyWorkerThread::GetRelativePath(const Slot *slot, int parent, const StringView &path)
{
	Array<const Slot*> dirs;

	dirs.Add(slot);

	while (slot->mParent != parent)
	{
		Slot* val;
		if (!mWatchSlots.TryGetValue(slot->mParent, &val))
			break;

		dirs.Add(slot);
		slot = val;
	}

	String result;
	for (uint32 count = dirs.Count(), i = count - 1; i < count; --i)
	{
		result.Append('/');
		result.Append(dirs[i]->mName);
	}

	if (!path.IsEmpty())
	{
		if (!result.IsEmpty())
			result.Append('/');

		result.Append(path);
	}

	return result;
}

void InotifyWorkerThread::DispatchEvent(const inotify_event *event, Array<const inotify_event *> unhandledEvents)
{
	// Watch removed
	if (event->mask & IN_IGNORED)
	{
		Slot slot;
		if (!mWatchSlots.Remove(event->wd, &slot))
			return;

		return;
	}

	Slot* slot;
	if (!mWatchSlots.TryGetValue(event->wd, &slot))
		return;


	if (event->mask & (IN_UNMOUNT))
	{
		slot->SendEvents(this, event, BfpFileChangeKind_Failed);
		return;
	}

	if (event->mask & IN_MOVED_FROM)
	{
		unhandledEvents.Add(event);
	}
	if ((event->mask & IN_MOVED_TO))
	{
		bool unhandled = true;
		for (int i = 0; i < unhandledEvents.size(); i++)
		{
			// Same cookie means the events are linked
			if (event->cookie != unhandledEvents[i]->cookie)
				continue;

			Slot* prevEventSlot;
			if (mWatchSlots.TryGetValue(unhandledEvents[i]->wd, &prevEventSlot))
			{
				break;
			}

			slot->SendEvents(this, event, BfpFileChangeKind_Renamed, unhandledEvents[i], prevEventSlot);
			unhandledEvents.RemoveAtFast(i);
			unhandled = false;
			break;
		}

		if (unhandled)
			unhandledEvents.Add(event);
	}

	if (event->mask & IN_CREATE)
	{
		slot->SendEvents(this, event, BfpFileChangeKind_Added);
		HandleDirAdd(event, slot, event->name, false);
	}
	if (event->mask & IN_DELETE)
	{
		slot->SendEvents(this, event, BfpFileChangeKind_Removed);
		// HandleDirRemove(event, slot, false);
	}
	if ((event->mask & IN_CLOSE_WRITE) || (event->mask & IN_ATTRIB))
	{
		slot->SendEvents(this, event, BfpFileChangeKind_Modified);
	}
}

void InotifyWorkerThread::DispatchUnhandledEvent(const inotify_event *event)
{
	Slot* slot;
	if (!mWatchSlots.TryGetValue(event->wd, &slot))
		return;

	if (event->mask & IN_MOVED_FROM)
	{
		slot->SendEvents(this, event, BfpFileChangeKind_Removed, NULL);
		// HandleDirRemove(event, slot, true);
	}
	if (event->mask & IN_MOVED_TO)
	{
		slot->SendEvents(this, event, BfpFileChangeKind_Added, NULL);
		HandleDirAdd(event, slot, event->name, true);
	}
}

WatchHandle InotifyWorkerThread::InotifyWatchPath(const String &path, Slot** resultSlot)
{
	uint32 additionalMask = 0;
	if (resultSlot != NULL)
		additionalMask |= IN_ONLYDIR;

#ifndef BFP_INOTIFY_FOLLOW_LINKS
	additionalMask |= IN_DONT_FOLLOW;
#endif

	int wd = inotify_add_watch(mInotifyHandle, path.c_str(), (IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVE) | additionalMask);
	if (wd == -1)
		return WatchHandle::Invalid();

	if (resultSlot == NULL)
		return { wd };

	int* keyPtr;
	Slot* slot;
	if (mWatchSlots.TryAdd(wd, &keyPtr, &slot))
	{
	}
	slot->mWd = wd;
	*resultSlot = slot;

	return {wd };
}

void InotifyWorkerThread::InotifyRemoveDirectoryWatcher(BfpFileWatcher *fileWatch)
{
	Slot* slot;
	if (!mWatchSlots.TryGetValue(fileWatch->mHandle.mIdx, &slot))
		return;

	slot->RemoveWatcher(fileWatch);

	if ((slot->mWatchers.IsEmpty()))
	{
		if ((slot->mWd != -1) && (inotify_rm_watch(mInotifyHandle, slot->mWd) == -1))
		{
			WATCHER_ERRPRINTF("Failed to remove watch handle(%d) err(%d)\n", handle, errno);
		}

		mWatchSlots.Remove(fileWatch->mHandle.mIdx);
	}

}

void InotifyWorkerThread::WatchSubdirectories(const String& path, Slot* slot, bool sendEvents, int parentWd)
{
	DIR* dirp = opendir(path.c_str());
	if (dirp == NULL)
		return;

	Array<DIR*> workList;
	String currentPath(path);

	workList.Add(dirp);

	int currentParentWd = parentWd;
	Array<int> parents;
	parents.Add(currentParentWd);

	while (workList.size() > 0)
	{
		DIR* current = workList.front();

		struct dirent* dp;
		while ((dp = readdir(current)) != NULL)
		{
			if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
				continue;

			// Send events for files/dirs inside the directory as don't receive events in newly created directories
			if (sendEvents)
			{
				for (auto watcher : slot->mWatchers)
				{
					if ((watcher->mFlags & BfpFileWatcherFlag_IncludeSubdirectories) == 0)
						continue;

					String localPath = currentPath.Substring(std::min(currentPath.length(), watcher->mPath.length()+1));
					localPath.Append('/');
					localPath.Append(dp->d_name);

					watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Added, watcher->mPath.c_str(), localPath.c_str(), NULL);
					watcher->mDirectoryChangeFunc(watcher, watcher->mUserData, BfpFileChangeKind_Modified, watcher->mPath.c_str(), localPath.c_str(), NULL);
				}
			}

			if (dp->d_type != DT_DIR)
				continue;

			const auto length = currentPath.length();
			currentPath.Append('/');
			currentPath.Append(dp->d_name);
			Slot* resultSlot;
			WatchHandle watchHandle = InotifyWatchPath(currentPath, &resultSlot);
			if (watchHandle.IsValid())
			{
				currentPath.RemoveToEnd(length);
				WATCHER_ERRPRINTF("Failed to add watch for subdirectory '%s' (%d)\n", o_path.c_str(), errno);
				continue;
			}
			resultSlot->CopyRecursiveWatchersFromSlot(slot);

			DIR* todo = opendir(currentPath.c_str());
			if (todo == NULL)
			{
				currentPath.RemoveToEnd(length);
				continue;
			}
			parents.Add(watchHandle.mIdx);
			workList.Add(todo);
		}

		workList.RemoveAt(0);
		closedir(dirp);
		auto dirSeparator = currentPath.LastIndexOf('/');
		if (dirSeparator == -1)
		{
			BF_ASSERT(workList.IsEmpty());
			return;
		}
		currentPath.RemoveToEnd(dirSeparator);
		currentParentWd = parents.GetFirstSafe();
		parents.RemoveAt(0);
	}
}
static void* WorkerProcThunk(void* _this)
{
	BfpThread_SetName(NULL, "InotifyFileWatcher", NULL);
	((InotifyWorkerThread*)_this)->WorkerProc();
	return NULL;
}

bool InotifyWorkerThread::Init()
{
	mInotifyHandle = inotify_init1(IN_CLOEXEC);
	if (mInotifyHandle == -1)
	{
		WATCHER_ERRPRINTF("Failed to initialize inotify (%d)\n", errno);
		return false;
	}

	if (pipe2(mShutdownPipe, O_CLOEXEC) == -1)
	{
		WATCHER_ERRPRINTF("Failed to create shutdown pipe (%d)\n", errno);
		return false;
	}

	int err = pthread_create(&mWorkerThread, NULL, &WorkerProcThunk, this);
	if (err != 0)
	{
		WATCHER_ERRPRINTF("Failed to create worker thread for inotify FileWatcher!\n");
		return false;
	}

	return true;
}

void InotifyWorkerThread::Shutdown()
{
	mShuttingDown = true;

	if (mShutdownPipe[1] != -1)
	{
		const char shutdown = 1;
		ssize_t written;
		do
		{
			written = write(mShutdownPipe[1], &shutdown, sizeof(shutdown));
		} while (written == -1 && errno == EINTR);
	}

	if ((mWorkerThread != NULL))
	{
		pthread_join(mWorkerThread, NULL);
		mWorkerThread = NULL;
	}

	for (auto watcher : mDeferredWatchers)
		delete watcher;
	mDeferredWatchers.Clear();

	if (mInotifyHandle != -1)
	{
		close(mInotifyHandle);
		mInotifyHandle = -1;
	}

	if (mShutdownPipe[0] != -1)
	{
		close(mShutdownPipe[0]);
		mShutdownPipe[0] = -1;
	}
	if (mShutdownPipe[1] != -1)
	{
		close(mShutdownPipe[1]);
		mShutdownPipe[1] = -1;
	}
}

bool InotifyFileWatchManager::Init()
{
	bool result = mWorkerThread.Init();
	mInitializedEvent.Set();
	return result;
}

void InotifyFileWatchManager::Shutdown()
{
	mWorkerThread.Shutdown();
}

BfpFileWatcher * InotifyFileWatchManager::WatchDirectory(const char *path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void *userData, BfpFileResult *outResult)
{
	mInitializedEvent.WaitFor();

	WatchHandle watchHandle = mWorkerThread.AddWatchPath(path);
	if (!watchHandle.IsValid())
	{
		WATCHER_ERRPRINTF("Failed to add watch for directory '%s' (%d)\n", path, errno);
		OUTRESULT(BfpFileResult_UnknownError);
		return NULL;
	}

	String watchPath;
	// Make watch path lexically absolute, so it won't change when working directory changes
	if (path[0] != '/')
	{
		char* cwdPtr = getcwd(NULL, 0);
		if (cwdPtr)
		{
			String cwdPath = String::MakeRef(cwdPtr);
			watchPath = GetAbsPath(path, cwdPath);
			free(cwdPtr);
		}
		else
		{
			OUTRESULT(BfpFileResult_NotFound);
			return NULL;
		}
	}
	else
	{
		watchPath = GetAbsPath(path, "/");
	}


	BfpFileWatcher* fileWatcher = new BfpFileWatcher();
	fileWatcher->mPath = watchPath;
	fileWatcher->mDirectoryChangeFunc = callback;
	fileWatcher->mHandle = watchHandle;
	fileWatcher->mFlags = flags;
	fileWatcher->mUserData = userData;
	fileWatcher->mMarkedForDeletion = false;

	mWorkerThread.AddWatcher(fileWatcher);

	OUTRESULT(BfpFileResult_Ok);
	return fileWatcher;
}

void InotifyFileWatchManager::Remove(BfpFileWatcher *watcher)
{
	mWorkerThread.DeleteWatcher(watcher);
}

FileWatchManager* FileWatchManager::Allocate()
{
    return new InotifyFileWatchManager();
}

#endif // BFP_HAS_FILEWATCHER
