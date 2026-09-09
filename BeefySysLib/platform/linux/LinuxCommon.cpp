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
#define WATCHER_ERRPRINTF(...)
#endif

struct WatchHandle
{
	uint32 mIdx;
	uint32 mGen;
};

struct BfpFileWatcher
{
	struct SubdirData
	{
		String mName;
		int mParentWd = -1;

		SubdirData() = default;

		SubdirData(const StringView& name, int parentWd) :
			mName(name),
			mParentWd(parentWd)
		{

		}
	};

    String mPath;
    BfpDirectoryChangeFunc mDirectoryChangeFunc;
    WatchHandle mHandle;
    BfpFileWatcherFlags mFlags;
    void* mUserData;

	// Use reference counting so it's safe to Remove Watcher inside mDirectoryChangeFunc
	int32 mRefCount = 1;
    Dictionary<int, SubdirData> mSubdirs;

	void Reference()
	{
		__sync_add_and_fetch(&mRefCount, 1);
	}

	void Release()
	{
		if (__sync_sub_and_fetch(&mRefCount, 1) == 0)
			delete this;
	}

	String GetSubdirPath(int wd)
	{
		if (wd == -1)
			return {};

		SubdirData* sd;
		if (!mSubdirs.TryGetValue(wd, &sd))
			return {};

		String path = GetSubdirPath(sd->mParentWd);
		path.Append(sd->mName);
		path.Append("/");
		return path;
	}

	String GetRelativePath(int wd, const StringView& eventPath)
	{
		String subdir = GetSubdirPath(wd);
		subdir.Append(eventPath);
		return subdir;
	}

	bool IsInSubdir(int wd, int target)
	{
		if ((target == wd) || (wd == -1))
			return false;

		SubdirData* sd;
		if (!mSubdirs.TryGetValue(wd, &sd))
			return false;

		if (sd->mParentWd == wd)
			return true;

		return IsInSubdir(sd->mParentWd, target);
	}

	void AddSubdirEntry(int handle, const StringView& name, int parentWd)
	{
		mSubdirs[handle] = SubdirData(name, parentWd);
	}

};


class InotifyFileWatchManager : public FileWatchManager
{
	struct Slot
	{
		int mWd;
		uint32 mGen;
		uint32 mRefs;
		uint32 mParent;
		uint32 mFirstChild;
		uint32 mNextSibling;
		String mName;
	};

    static constexpr size_t MAX_NOTIFY_EVENTS = 64;
    static constexpr size_t NOTIFY_BUFFER_SIZE = (MAX_NOTIFY_EVENTS * (sizeof(inotify_event) + PATH_MAX));

    int mInotifyHandle = -1;
	int mShutdownPipe[2] = { -1, -1 };
	volatile bool mShuttingDown = false;
    pthread_t mWorkerThread = NULL;
    Dictionary<int, BfpFileWatcher*> mWatchers;
    CritSect mCritSect;
    alignas(inotify_event) char mEventBuffer[NOTIFY_BUFFER_SIZE];


private:

	void DispatchEvent(const inotify_event* event, Array<const inotify_event*> unhandledEvents)
	{
		BfpFileWatcher* w;
        {
            AutoCrit autoCrit(mCritSect);

            // Watch removed
            if (event->mask & IN_IGNORED)
            {
                mWatchers.Remove(event->wd);
                return;
            }

            if (!mWatchers.TryGetValue(event->wd, &w))
            	return;

            w->Reference();

        }
        defer( w->Release() );

        String relPath = w->GetRelativePath(event->wd, event->name);

        if (event->mask & (IN_Q_OVERFLOW | IN_UNMOUNT))
        {
            w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Failed, w->mPath.c_str(), NULL, NULL);
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

                int subdirWd = -1;
                if ((event->mask & IN_ISDIR) && (w->mFlags & BfpFileWatcherFlag_IncludeSubdirectories))
                {
                    String prevName = String::MakeRef(unhandledEvents[i]->name);
                    for (const auto& kv : w->mSubdirs)
                    {
                    	if ((kv.mValue.mParentWd == unhandledEvents[i]->wd) && (kv.mValue.mName == prevName))
                    	{
                    		subdirWd = kv.mKey;
                    		break;
                    	}
                    }
                }

                // Only handle as rename if src and dst directory is the same
                // Otherwise emit remove / add
                if (event->wd == unhandledEvents[i]->wd)
                {
                    String prevName = w->GetRelativePath(unhandledEvents[i]->wd, unhandledEvents[i]->name);
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Renamed, w->mPath.c_str(), prevName.c_str(), relPath.c_str());
                }
                else
                {
                    String oldPath = w->GetRelativePath(unhandledEvents[i]->wd, unhandledEvents[i]->name);
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Removed, w->mPath.c_str(), oldPath.c_str(), NULL);
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Added, w->mPath.c_str(), relPath.c_str(), NULL);
                }

                if (subdirWd != -1)
                    w->AddSubdirEntry(subdirWd, event->name, event->wd);

                unhandledEvents.RemoveAtFast(i);
                unhandled = false;
                break;
            }

            if (unhandled)
                unhandledEvents.Add(event);
        }

        if (event->mask & IN_CREATE)
        {
            w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Added, w->mPath.c_str(), relPath.c_str(), NULL);
            HandleDirAdd(event, w, relPath, false);
        }
        if (event->mask & IN_DELETE)
        {
            w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Removed, w->mPath.c_str(), relPath.c_str(), NULL);
            HandleDirRemove(event, w, false);
        }
        if ((event->mask & IN_CLOSE_WRITE) || (event->mask & IN_ATTRIB))
        {
            w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Modified, w->mPath.c_str(), relPath.c_str(), NULL);
        }
	}

	void DispatchUnhandledEvent(const inotify_event* event)
	{
		BfpFileWatcher* w;
		{
			AutoCrit autoCrit(mCritSect);
			if (!mWatchers.TryGetValue(event->wd, &w))
				return;

			w->Reference();
		}
		defer( w->Release() );

		String relPath = w->GetRelativePath(event->wd, event->name);

		if (event->mask & IN_MOVED_FROM)
		{
			w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Removed, w->mPath.c_str(), relPath.c_str(), NULL);
			HandleDirRemove(event, w, true);
		}
		if (event->mask & IN_MOVED_TO)
		{
			w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Added, w->mPath.c_str(), relPath.c_str(), NULL);
			HandleDirAdd(event, w, relPath, true);
		}
	}

    void WorkerProc()
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
        }
    }

    static void* WorkerProcThunk(void* _this)
    {
        BfpThread_SetName(NULL, "InotifyFileWatcher", NULL);
        ((InotifyFileWatchManager*)_this)->WorkerProc();
        return NULL;
    }
   
    void HandleDirRemove(const inotify_event* event, BfpFileWatcher* fileWatch, bool moved)
    {
    	if ((event->mask & IN_ISDIR) == 0)
    		return;

    	if ((fileWatch->mFlags & BfpFileWatcherFlag_IncludeSubdirectories) == 0)
    		return;

    	const bool isSubdir = (event->wd == fileWatch->mHandle);

    	// If we are not inside a subdir that means we are the root
		// inotify watchers are automatically removed
    	if ((!isSubdir) && (!moved))
			return;
        
        AutoCrit autoCrit(mCritSect);
    	Array<int> toRemove;
        for (const auto& kv : fileWatch->mSubdirs)
        {
        	if (isSubdir && fileWatch->IsInSubdir(kv.mKey, event->wd))
        	{
        		toRemove.Add(kv.mKey);
        	}
        }

    	for (int handle : toRemove)
    	{
        	fileWatch->mSubdirs.Remove(handle);
    		InotifyRemoveWatch(handle);
    	}
    }

    void HandleDirAdd(const inotify_event* event, BfpFileWatcher* fileWatch, const String& relPath, bool moved)
    {
    	if ((event->mask & IN_ISDIR) == 0)
    		return;

        if (!(fileWatch->mFlags & BfpFileWatcherFlag_IncludeSubdirectories))
            return;

	    // Check if watcher was removed in callback
        {
	        AutoCrit autoCrit(mCritSect);
        	if (!mWatchers.ContainsKey(event->wd))
        		return;
        }

        int watchHandle = InotifyWatchPath(relPath.c_str());
        if (watchHandle == -1)
        {
            WATCHER_ERRPRINTF("Failed to add watch for subdirectory '%s' (%d)\n", dirPath.c_str(), errno);
            return;
        }
        AddWatchEntry(watchHandle, fileWatch);
        fileWatch->AddSubdirEntry(watchHandle, relPath, event->wd);
        WatchSubdirectories(relPath.c_str(), fileWatch, !moved, event->wd);
    }

    void AddWatchEntry(int handle, BfpFileWatcher* fileWatcher)
    {
        mWatchers[handle] = fileWatcher;
    }

    WatchHandle InotifyWatchPath(const String& path)
    {
        int wd = inotify_add_watch(mInotifyHandle, path.c_str(), IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVE);
		return wd;
    }

    void InotifyRemoveWatch(int handle)
    {
    	if ((!mWatchers.Remove(handle)))
			return;

        if (inotify_rm_watch(mInotifyHandle, handle) == -1)
        {
            WATCHER_ERRPRINTF("Failed to remove watch handle(%d) err(%d)\n", handle, errno);
        }
    }

    void WatchSubdirectories(const String& path, BfpFileWatcher* fileWatcher, bool sendEvents, int parentWd)
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
        			String localPath = currentPath.Substring(std::min(currentPath.length(), fileWatcher->mPath.length()+1));
        			localPath.Append('/');
        			localPath.Append(dp->d_name);
        			fileWatcher->mDirectoryChangeFunc(fileWatcher, fileWatcher->mUserData, BfpFileChangeKind_Added, fileWatcher->mPath.c_str(), localPath.c_str(), NULL);
        			fileWatcher->mDirectoryChangeFunc(fileWatcher, fileWatcher->mUserData, BfpFileChangeKind_Modified, fileWatcher->mPath.c_str(), localPath.c_str(), NULL);
        		}

        		if (dp->d_type != DT_DIR)
        			continue;

        		const auto length = currentPath.length();
        		currentPath.Append('/');
        		currentPath.Append(dp->d_name);
        		int watchHandle = InotifyWatchPath(currentPath.c_str());
        		if (watchHandle == -1)
        		{
        			currentPath.RemoveToEnd(length);
        			WATCHER_ERRPRINTF("Failed to add watch for subdirectory '%s' (%d)\n", o_path.c_str(), errno);
        			continue;
        		}
        		AddWatchEntry(watchHandle, fileWatcher);
        		fileWatcher->AddSubdirEntry(watchHandle, currentPath, currentParentWd);
        		DIR* todo = opendir(currentPath.c_str());
        		if (todo == NULL)
        		{
        			currentPath.RemoveToEnd(length);
        			continue;
        		}
        		parents.Add(watchHandle);
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

public:

	bool Init() override
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

	void Shutdown() override
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

    virtual BfpFileWatcher* WatchDirectory(const char* path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void* userData, BfpFileResult* outResult) override
    {
        int watchHandle = InotifyWatchPath(path);
        if (watchHandle == -1)
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
        AddWatchEntry(watchHandle, fileWatcher);

        if (flags & BfpFileWatcherFlag_IncludeSubdirectories)
        {
            WatchSubdirectories(path, fileWatcher, false, -1);
        }

        return fileWatcher;
    }

	void Remove(BfpFileWatcher* watcher) override
    {
        AutoCrit autoCrit(mCritSect);

        if ((watcher->mFlags & BfpFileWatcherFlag_IncludeSubdirectories))
        {
            for (const auto& subdir : watcher->mSubdirs)
            {
            	InotifyRemoveWatch(subdir.mKey);
            }
        }

		InotifyRemoveWatch(watcher->mHandle);
		watcher->Release();
    }

};

FileWatchManager* FileWatchManager::Allocate()
{
    return new InotifyFileWatchManager();
}

#endif // BFP_HAS_FILEWATCHER
