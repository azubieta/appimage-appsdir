// system includes
#include <iostream>
#include <map>
#include <sys/inotify.h>
#include <unistd.h>

// library includes
#include <QDir>
#include <QTimer>
#include <QThread>
#include <QDebug>

// local includes
#include "FileSystemWatcher.h"

class INotifyEvent {
public:
    uint32_t mask;
    QString path;

public:
    INotifyEvent(uint32_t mask, QString path) : mask(mask), path(std::move(path)) {}
};

class FileSystemWatcher::PrivateData {
public:
    enum EVENT_TYPES {
        // events that indicate file creations, modifications etc.
                fileChangeEvents = IN_CLOSE_WRITE | IN_MOVED_TO,
        // events that indicate a file removal from a directory, e.g., deletion or moving to another location
                fileRemovalEvents = IN_DELETE | IN_MOVED_FROM,
    };

public:
    QSet<QString> watchedDirectories;
    QTimer eventsLoopTimer;

private:
    int fd = -1;
    std::map<int, QString> watchFdMap;

public:
    // reads events from the inotify fd and emits the correct signals
    std::vector<INotifyEvent> readEventsFromFd() {
        // read raw bytes into buffer
        // this is necessary, as the inotify_events have dynamic sizes
        static const auto bufSize = 4096;
        char buffer[bufSize] __attribute__ ((aligned(8)));

        const auto rv = read(fd, buffer, bufSize);
        const auto error = errno;

        if (rv == 0) {
            throw FileSystemWatcherError("read() on inotify FD must never return 0");
        }

        if (rv == -1) {
            // we're using a non-blocking inotify fd, therefore, if errno is set to EAGAIN, we just didn't find any
            // new events
            // this is not an error case
            if (error == EAGAIN)
                return {};

            throw FileSystemWatcherError(QString("Failed to read from inotify fd: ") + strerror(error));
        }

        // read events into vector
        std::vector<INotifyEvent> events;

        for (char* p = buffer; p < buffer + rv;) {
            // create inotify_event from current position in buffer
            auto* currentEvent = (struct inotify_event*) p;

            // initialize new INotifyEvent with the data from the currentEvent
            QString relativePath(currentEvent->name);
            auto directoryPath = watchFdMap[currentEvent->wd];
            events.emplace_back(currentEvent->mask, directoryPath + "/" + relativePath);

            // update current position in buffer
            p += sizeof(struct inotify_event) + currentEvent->len;
        }

        return events;
    }

    PrivateData() : watchedDirectories() {
        fd = inotify_init1(IN_NONBLOCK);
        if (fd < 0) {
            auto error = errno;
            throw FileSystemWatcherError(strerror(error));
        }
    };

    bool startWatching() {
        static const auto mask = fileChangeEvents | fileRemovalEvents;

        for (const auto& directory : watchedDirectories) {
            if (!QDir(directory).exists()) {
                qWarning() << "Warning: directory " << directory << "does not exist, skipping";
                continue;
            }

            const int watchFd = inotify_add_watch(fd, directory.toStdString().c_str(), mask);

            if (watchFd == -1) {
                const auto error = errno;
                qWarning() << "Failed to start watching: " << strerror(error);
                return false;
            }

            watchFdMap[watchFd] = directory;
            eventsLoopTimer.start();
        }

        return true;
    }

    bool stopWatching() {
        while (!watchFdMap.empty()) {
            const auto pair = *(watchFdMap.begin());
            const auto watchFd = pair.first;

            if (inotify_rm_watch(fd, watchFd) == -1) {
                const auto error = errno;
                qWarning() << "Failed to stop watching: " << strerror(error);
                return false;
            }

            watchFdMap.erase(watchFd);
            eventsLoopTimer.stop();
        }

        return true;
    }
};

FileSystemWatcher::FileSystemWatcher() {
    d = std::make_shared<PrivateData>();

    d->eventsLoopTimer.setInterval(100);
    connect(&d->eventsLoopTimer, &QTimer::timeout, this, &FileSystemWatcher::readEvents);
}

FileSystemWatcher::FileSystemWatcher(const QString& path) : FileSystemWatcher() {
    if (!QDir(path).exists())
        QDir().mkdir(path);
    d->watchedDirectories.insert(path);
}

FileSystemWatcher::FileSystemWatcher(const QSet<QString>& paths) : FileSystemWatcher() {
    d->watchedDirectories = paths;
}

QSet<QString> FileSystemWatcher::directories() {
    return d->watchedDirectories;
}

bool FileSystemWatcher::enable() {
    return d->startWatching();
}

bool FileSystemWatcher::disable() {
    return d->stopWatching();
}

void FileSystemWatcher::readEvents() {
    auto events = d->readEventsFromFd();

    for (const auto& event : events) {
        const auto mask = event.mask;

        if (mask & d->fileChangeEvents) {
            emit fileChanged(event.path);
        } else if (mask & d->fileRemovalEvents) {
            emit fileRemoved(event.path);
        }
    }
}
