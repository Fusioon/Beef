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

// The standard callback
void FSEventsCallback(
    ConstFSEventStreamRef streamRef,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[])
{
    char **paths = (char **)eventPaths;
    for (size_t i = 0; i < numEvents; i++) {
        //std::cout << "[FSEvents] Change detected at: " << paths[i] << std::endl;
    }
}

struct BfpFileWatcher
{

};

class FsEventFileWatchManager : public FileWatchManager
{
public:
    BfpThread* mWatcherThread;

public:
    bool Init() override;

    void Shutdown() override;

    BfpFileWatcher* WatchDirectory(const char *path, BfpDirectoryChangeFunc callback, BfpFileWatcherFlags flags, void *userData, BfpFileResult *outResult) override;

    void Remove(BfpFileWatcher *watcher) override;

    void WorkerThreadProc();
};

void FsEventFileWatchManager::WorkerThreadProc()
{

}

static void WatcherWorkThreadThunk(void* userData)
{
    ((FsEventFileWatchManager*)userData)->WorkerThreadProc();
}

bool FsEventFileWatchManager::Init()
{
    mWatcherThread = BfpThread_Create(&WatcherWorkThreadThunk, this);
    if (mWatcherThread == NULL)
        return false;

    BfpThread_Yield();
}

void FsEventFileWatchManager::Shutdown()
{

}

BfpFileWatcher* FsEventFileWatchManager::WatchDirectory(const char* path, BfpDirectoryChangeFunc callback,
    BfpFileWatcherFlags flags, void* userData, BfpFileResult* outResult)
{
    // Wait until the background thread has fully spun up its Run Loop
    while (bgRunLoop.load() == nullptr)
    {
        std::this_thread::yield();
    }

    CFRunLoopRef targetRunLoop = bgRunLoop.load();

    // Prepare the path
    CFStringRef mypath = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&mypath, 1, NULL);
    FSEventStreamContext context = {0, NULL, NULL, NULL, NULL};

    // Create a NEW stream for this specific directory
    FSEventStreamRef newStream = FSEventStreamCreate(
        NULL, &FSEventsCallback, &context, pathsToWatch,
        kFSEventStreamEventIdSinceNow, 0.2,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer
    );

    // Attach the new stream to the BACKGROUND thread's Run Loop
    FSEventStreamScheduleWithRunLoop(newStream, targetRunLoop, kCFRunLoopDefaultMode);

    // Start the stream
    FSEventStreamStart(newStream);

    // Wake up the background Run Loop to ensure it immediately registers the new stream
    CFRunLoopWakeUp(targetRunLoop);

    // Cleanup references (the stream retains what it needs)
    CFRelease(mypath);
    CFRelease(pathsToWatch);
}

void FsEventFileWatchManager::Remove(BfpFileWatcher* watcher)
{
}

FileWatchManager* FileWatchManager::Allocate()
{
    return new FsEventFileWatchManager();
}

#endif // BFP_HAS_FILEWATCHER
