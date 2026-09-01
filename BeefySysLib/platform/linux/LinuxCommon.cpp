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

struct BfpFileWatcher
{
	struct SubdirData
	{
		String mName;
		int mParentWd = -1;
	};

    String mPath;
    BfpDirectoryChangeFunc mDirectoryChangeFunc;
    int mHandle;
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
		SubdirData* sd;
		if (!mSubdirs.TryGetValue(wd, &sd))
			return {};

		return GetPathRecursive(sd);
	}

	static String GetPathRecursive(const SubdirData* sd)
	{
		String path;
		if (sd->mParentWd != -1)
			path = GetPathRecursive(sd->mParent);

		path.Append(sd->mName);
		path.Append("/");
		return path;
	}
};


class InotifyFileWatchManager : public FileWatchManager
{
    static constexpr size_t MAX_NOTIFY_EVENTS = 64;
    static constexpr size_t NOTIFY_BUFFER_SIZE = (MAX_NOTIFY_EVENTS * (sizeof(inotify_event) + PATH_MAX));

    int mInotifyHandle = -1;
	int mShutdownPipe[2] = {-1, -1};
    pthread_t mWorkerThread = NULL;
    Dictionary<int, BfpFileWatcher*> mWatchers;
    CritSect mCritSect;
    char mEventBuffer[NOTIFY_BUFFER_SIZE];

private:

    void WorkerProc()
    {
        char pathBuffer[PATH_MAX];

    	struct pollfd fds[2];
    	fds[0].fd = mInotifyHandle;
    	fds[0].events = POLLIN;
    	fds[1].fd = mShutdownPipe[0];
    	fds[1].events = POLLIN;

        Array<inotify_event*> unhandledEvents;
        while (true)
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
            	defer ( pos += sizeof(inotify_event) + event->len );
                if(event->len == 0)
					continue;

                BfpFileWatcher* w;
            	String subdir;
                {
                    AutoCrit autoCrit(mCritSect);

                	// Watch removed
                	if (event->mask & IN_IGNORED)
                	{
                		mWatchers.Remove(event->wd);
                		continue;
                	}

                    if (!mWatchers.TryGetValue(event->wd, &w))
                        continue;

                	w->Reference();
                	subdir = w->GetSubdirPath(event->wd);
                }
            	defer( w->Release() );

            	if (event->mask & (IN_Q_OVERFLOW | IN_UNMOUNT))
            	{
            		w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Failed, w->mPath.c_str(), NULL, NULL);
            		continue;
            	}

                if (GetRelativePath(pathBuffer, sizeof(pathBuffer), event->name, event->len, w, subdir) == 0)
                {
                    // our buffer was too small, we can't handle this event
                    continue;
                }

                if (event->mask & IN_MOVED_FROM)
                {
                    unhandledEvents.Add(event);
                }
                if ((event->mask & IN_MOVED_TO))
                {
                    bool handled = false;
                    for (int i = 0; i < unhandledEvents.size(); i++)
                    {
                        // Only handle as rename if src and dst directory is the same
                        if ((event->cookie == unhandledEvents[i]->cookie) && (event->wd == unhandledEvents[i]->wd))
                        {
                            char renameBuffer[PATH_MAX];
                            if (GetRelativePath(renameBuffer, sizeof(renameBuffer), unhandledEvents[i]->name, unhandledEvents[i]->len, w, subdir) == 0)
                            {
                               break;
                            }
                            w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Renamed, w->mPath.c_str(), renameBuffer, pathBuffer);
                            unhandledEvents.RemoveAtFast(i);
                            handled = true;
                            break;
                        }
                    }

                    if (!handled)
                    {
                        unhandledEvents.Add(event);
                    }
                }

                if (event->mask & IN_CREATE)
                {
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Added, w->mPath.c_str(), pathBuffer, NULL);
                    HandleDirAdd(event, w, subdir, false);
                }
                if (event->mask & IN_DELETE)
                {
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Removed, w->mPath.c_str(), pathBuffer, NULL);
                    HandleDirRemove(event, w, subdir);
                }
                if ((event->mask & IN_CLOSE_WRITE) || (event->mask & IN_ATTRIB))
                {
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Modified, w->mPath.c_str(), pathBuffer, NULL);
                }
            }

            for (auto event : unhandledEvents)
            {
                BfpFileWatcher* w;
            	String subdir;

                {
                    AutoCrit autoCrit(mCritSect);
                    if (!mWatchers.TryGetValue(event->wd, &w))
                        continue;
                	w->Reference();
                	subdir = w->GetSubdirPath(event->wd);
                }
            	defer (w->Release());

                if (GetRelativePath(pathBuffer, sizeof(pathBuffer), event->name, event->len, w, subdir) == 0)
                {
                    continue;
                }

                if (event->mask & IN_MOVED_FROM)
                {
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Removed, w->mPath.c_str(), pathBuffer, NULL);
                    HandleDirRemove(event, w, subdir);
                }
                if (event->mask & IN_MOVED_TO)
                {
                    w->mDirectoryChangeFunc(w, w->mUserData, BfpFileChangeKind_Added, w->mPath.c_str(), pathBuffer, NULL);
                    HandleDirAdd(event, w, subdir, true);
                }

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
   
    void HandleDirRemove(const inotify_event* event, BfpFileWatcher* fileWatch, const StringView& subdir)
    {
        const bool shouldHandle = (event->mask & IN_ISDIR) && (fileWatch->mFlags & BfpFileWatcherFlag_IncludeSubdirectories);
        if (!shouldHandle)
            return;
        
        AutoCrit autoCrit(mCritSect);
        String removedDir;
        if (!subdir.IsEmpty())
        {
            removedDir = subdir;
            removedDir.Append('/');
            removedDir.Append(event->name);
        }

    	Array<int> toRemove;
        for (const auto& kv : fileWatch->mSubdirs)
        {
        	if ((subdir.IsEmpty()) || (kv.mValue.GetPath().StartsWith(removedDir)))
        	{
        		toRemove.Add(kv.mKey);
        	}
        }

    	for (int handle : toRemove)
    	{
        	fileWatch->mSubdirs.Remove(handle);
    		if (mWatchers.Remove(handle))
    			InotifyRemoveWatch(handle);
    	}
    }

    void HandleDirAdd(const inotify_event* event, BfpFileWatcher* fileWatch, const StringView& subdir, bool wasMoved)
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

        String dirPath = fileWatch->mPath;

        if (!subdir.IsEmpty())
        {
            dirPath.Append('/');
            dirPath.Append(subdir);
        }
        dirPath.Append('/');
        dirPath.Append(event->name);

        int watchHandle = InotifyWatchPath(dirPath.c_str());
        if (watchHandle == -1)
        {
            WATCHER_ERRPRINTF("Failed to add watch for subdirectory '%s' (%d)\n", dirPath.c_str(), errno);
            return;
        }
        AddWatchEntry(watchHandle, fileWatch);
        AddSubdirEntry(watchHandle, dirPath, fileWatch, );
        WatchSubdirectories(dirPath.c_str(), fileWatch, !wasMoved);
    }

    void AddWatchEntry(int handle, BfpFileWatcher* fileWatcher)
    {
        AutoCrit autoCrit(mCritSect);
        mWatchers[handle] = fileWatcher;
    }

    void AddSubdirEntry(int handle, const StringView& name, BfpFileWatcher* fileWatcher, BfpFileWatcher::SubdirData* subDirData)
    {
        AutoCrit autoCrit(mCritSect);
    	BfpFileWatcher::SubdirData subdirData;
    	subdirData.mName = name;
    	subdirData.mParent = subDirData;
        fileWatcher->mSubdirs[handle] = subdirData;
    }

    int InotifyWatchPath(const char* path)
    {
        return inotify_add_watch(mInotifyHandle, path, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVE);
    }

    void InotifyRemoveWatch(int handle)
    {
        if (inotify_rm_watch(mInotifyHandle, handle) == -1)
        {
            WATCHER_ERRPRINTF("Failed to remove watch handle(%d) err(%d)\n", handle, errno);
        }
    }

    void HandleDirectory(DIR* dirp, String& o_path, Array<DIR*>& o_workList, BfpFileWatcher* fileWatcher, bool sendEvents)
    {
        struct dirent* dp;
        while ((dp = readdir(dirp)) != NULL)
        {
            if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
                continue;

            // Send events for files/dirs inside the directory as don't receive events in newly created directories 
            if (sendEvents)
            {
                String localPath = o_path.Substring(std::min(o_path.length(), fileWatcher->mPath.length()+1));
                localPath.Append('/');
                localPath.Append(dp->d_name);
                fileWatcher->mDirectoryChangeFunc(fileWatcher, fileWatcher->mUserData, BfpFileChangeKind_Added, fileWatcher->mPath.c_str(), localPath.c_str(), NULL);
                fileWatcher->mDirectoryChangeFunc(fileWatcher, fileWatcher->mUserData, BfpFileChangeKind_Modified, fileWatcher->mPath.c_str(), localPath.c_str(), NULL);
            }

            if (dp->d_type != DT_DIR)
                continue;

            const auto length = o_path.length();
            o_path.Append('/');
            o_path.Append(dp->d_name);
            int watchHandle = InotifyWatchPath(o_path.c_str());
            if (watchHandle == -1)
            {
                o_path.RemoveToEnd(length);
                WATCHER_ERRPRINTF("Failed to add watch for subdirectory '%s' (%d)\n", o_path.c_str(), errno);
                continue;
            }
            AddWatchEntry(watchHandle, fileWatcher);
            AddSubdirEntry(watchHandle, o_path, fileWatcher);
            DIR* todo = opendir(o_path.c_str());
            if (todo == NULL)
            {
                 o_path.RemoveToEnd(length);
                 continue;
            }
            o_workList.Add(dirp);
            o_workList.Add(todo);
            return;
        }
        o_workList.Add(NULL);
        closedir(dirp);
    }

    void WatchSubdirectories(const char* path, BfpFileWatcher* fileWatcher, bool sendEvents)
    {
        DIR* dirp = opendir(path);
        if (dirp == NULL)
            return;

        Array<DIR*> workList;
        String currentPath(path);

        HandleDirectory(dirp, currentPath, workList, fileWatcher, sendEvents);
        while (workList.size() > 0)
        {
            dirp = workList.back();
            workList.pop_back();
            if (dirp == NULL)
            {
                auto dirSeparator = currentPath.LastIndexOf('/');
                if (dirSeparator == -1)
                {
                    BF_ASSERT(workList.IsEmpty());
                    break;
                }
                currentPath.RemoveToEnd(dirSeparator);
                continue;
            }
            HandleDirectory(dirp, currentPath, workList, fileWatcher, sendEvents);
        }
    }

    static int GetRelativePath(char* buffer, int bufferSize, const char* fileName, int fileNameLength, const BfpFileWatcher* fileWatcher, const StringView& subdir)
    {
        if (subdir.IsEmpty())
        {
            memcpy(buffer, fileName, fileNameLength);
            return fileNameLength;
        }

        const auto subdirLength = subdir.length();
        if (bufferSize < (subdirLength + fileNameLength + 2))
            return 0;
        memcpy(buffer, subdir.mPtr, subdirLength);
        buffer[subdirLength] = '/';
        buffer += subdirLength + 1;
        memcpy(buffer, fileName, fileNameLength);
        buffer[fileNameLength] = '\0';
        return subdirLength + fileNameLength + 1;
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
		if (mShutdownPipe[1] != -1)
		{
			const char wake = 1;
			ssize_t written;
			do
			{
				written = write(mShutdownPipe[1], &wake, sizeof(wake));
			} while (written == -1 && errno == EINTR);
		}

		if (mWorkerThread != NULL)
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
        BfpFileWatcher* fileWatcher = new BfpFileWatcher();
        fileWatcher->mPath = path;
        fileWatcher->mDirectoryChangeFunc = callback;
        fileWatcher->mHandle = watchHandle;
        fileWatcher->mFlags = flags;
        fileWatcher->mUserData = userData;
        AddWatchEntry(watchHandle, fileWatcher);

        if (flags & BfpFileWatcherFlag_IncludeSubdirectories)
        {
            WatchSubdirectories(path, fileWatcher, false);
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
            	if (mWatchers.Remove(subdir.mKey))
            		InotifyRemoveWatch(subdir.mKey);
            }
        }

	    // Check if watched directory exists so we don't error/remove other watch
	    if ((DirectoryExists(watcher->mPath)) && (mWatchers.Remove(watcher->mHandle)))
	    {
	        InotifyRemoveWatch(watcher->mHandle);
	    }

		watcher->Release();
    }

};

FileWatchManager* FileWatchManager::Allocate()
{
    return new InotifyFileWatchManager();
}

#endif // BFP_HAS_FILEWATCHER
