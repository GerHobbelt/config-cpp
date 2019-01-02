#include "inotify.h"

#include <string>
#include <vector>
#include <iostream>
#include <cstring>

#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ConfigCpp {

Inotify::Inotify()
    : m_error(0)
    , m_eventTimeout(0)
    , m_lastEventTime(std::chrono::steady_clock::now())
    , m_eventMask(IN_ALL_EVENTS)
    , m_threadSleep(250)
    , m_inotifyFd(0)
    , m_onEventTimeout([](FileSystemEvent) {})
    , m_eventBuffer(MAX_EVENTS * (EVENT_SIZE + 16), 0)
    , m_pipeReadIdx(0)
    , m_pipeWriteIdx(1)
{
    m_stopped = false;

    if (pipe2(m_stopPipeFd, O_NONBLOCK) == -1) {
        m_error = errno;
        std::stringstream errorStream;
        errorStream << "Can't initialize stop pipe ! " << strerror(m_error) << ".";
        throw std::runtime_error(errorStream.str());
    }

    m_inotifyFd = inotify_init1(IN_NONBLOCK);
    if (m_inotifyFd == -1) {
        m_error = errno;
        std::stringstream errorStream;
        errorStream << "Can't initialize inotify ! " << strerror(m_error) << ".";
        throw std::runtime_error(errorStream.str());
    }

    m_epollFd = epoll_create1(0);
    if (m_epollFd  == -1) {
        m_error = errno;
        std::stringstream errorStream;
        errorStream << "Can't initialize epoll ! " << strerror(m_error) << ".";
        throw std::runtime_error(errorStream.str());
    }

    m_inotifyEpollEvent.events = EPOLLIN | EPOLLET;
    m_inotifyEpollEvent.data.fd = m_inotifyFd;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_inotifyFd, &m_inotifyEpollEvent) == -1) {
        m_error = errno;
        std::stringstream errorStream;
        errorStream << "Can't add inotify filedescriptor to epoll ! " << strerror(m_error) << ".";
        throw std::runtime_error(errorStream.str());
    }

    m_stopPipeEpollEvent.events = EPOLLIN | EPOLLET;
    m_stopPipeEpollEvent.data.fd = m_stopPipeFd[m_pipeReadIdx];
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_stopPipeFd[m_pipeReadIdx], &m_stopPipeEpollEvent) == -1) {
        m_error = errno;
        std::stringstream errorStream;
        errorStream << "Can't add pipe filedescriptor to epoll ! " << strerror(m_error) << ".";
        throw std::runtime_error(errorStream.str());
    }
}

Inotify::~Inotify()
{
    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, m_inotifyFd, 0);
    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, m_stopPipeFd[m_pipeReadIdx], 0);

    if (!close(m_inotifyFd)) {
        m_error = errno;
    }

    if (!close(m_epollFd)) {
        m_error = errno;
    }

    close(m_stopPipeFd[m_pipeReadIdx]);
    close(m_stopPipeFd[m_pipeWriteIdx]);
}

void Inotify::watchFile(const std::string &filePath)
{
    if (exists(filePath)) {
        m_error = 0;
        int wd = 0;
        wd = inotify_add_watch(m_inotifyFd, filePath.c_str(), m_eventMask);

        if (wd == -1) {
            m_error = errno;
            std::stringstream errorStream;
            if (m_error == 28) {
                errorStream << "Failed to watch! " << strerror(m_error)
                            << ". Please increase number of watches in "
                               "\"/proc/sys/fs/inotify/max_user_watches\".";
                throw std::runtime_error(errorStream.str());
            }

            errorStream << "Failed to watch! " << strerror(m_error)
                        << ". Path: " << filePath.c_str();
            throw std::runtime_error(errorStream.str());
        }
        m_directoryMap.insert({wd, filePath});
        m_watchMap.insert({filePath, wd});
    } 
}

void Inotify::unwatchFile(const std::string &file)
{
    removeWatch(m_watchMap[file]);
}

void Inotify::removeWatch(int wd)
{
    (void)inotify_rm_watch(m_inotifyFd, wd);
}

std::string Inotify::wdToPath(int wd)
{
    return m_directoryMap[wd];
}

void Inotify::onEvent(Event event, EventObserver observer)
{
    m_eventMask |= static_cast<std::uint32_t>(event);
    m_eventObserver[event] = observer;
}

void Inotify::onUnexpectedEvent(EventObserver observer)
{
    m_unexpectedEventObserver = observer;
}

void Inotify::setEventTimeout(
    std::chrono::milliseconds eventTimeout, std::function<void(FileSystemEvent)> onEventTimeout)
{
    m_lastEventTime -= eventTimeout;
    m_eventTimeout = eventTimeout;
    m_onEventTimeout = onEventTimeout;
}

bool Inotify::getNextEvent( FileSystemEvent &event) {
    std::vector<FileSystemEvent> newEvents;

    while (m_eventQueue.empty() && !m_stopped) {
        auto length = readEventsIntoBuffer(m_eventBuffer);
        readEventsFromBuffer(m_eventBuffer.data(), length, newEvents);
        filterEvents(newEvents, m_eventQueue);
    }

    if (m_stopped) {
        return false;
    }

    event = m_eventQueue.front();
    m_eventQueue.pop();
    return true;
}

void Inotify::stop()
{
    m_stopped = true;
    sendStopSignal();
}

void Inotify::start()
{
    m_thread = std::thread([this](){ this->run();});
}

void Inotify::run()
{
    std::cout << "Inotify::run()\n";
    while (true) {
        if (m_stopped) break;
    
        FileSystemEvent fileSystemEvent;
        if (!getNextEvent(fileSystemEvent)) {
            continue;
        }
    
        Event currentEvent = static_cast<Event>(fileSystemEvent.m_mask);
        bool dispatched = false;

        Notification notification { currentEvent, fileSystemEvent.m_path, fileSystemEvent.m_eventTime };

        for (auto& eventAndEventObserver : m_eventObserver) {
            auto& event = eventAndEventObserver.first;
            auto& eventObserver = eventAndEventObserver.second;
            if (ConfigCpp::containsEvent(event, currentEvent)) {
                eventObserver(notification);
                dispatched = true;
                break;
            }
        }

        if (!dispatched && m_unexpectedEventObserver) {
            m_unexpectedEventObserver(notification);
        }
    }

}

void Inotify::sendStopSignal()
{
    std::vector<std::uint8_t> buf(1,0);
    write(m_stopPipeFd[m_pipeWriteIdx], buf.data(), buf.size());
}

bool Inotify::hasStopped()
{
    return m_stopped;
}

bool Inotify::isOnTimeout(const std::chrono::steady_clock::time_point &eventTime)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(eventTime - m_lastEventTime) < m_eventTimeout;
}

ssize_t Inotify::readEventsIntoBuffer(std::vector<uint8_t>& eventBuffer)
{
    ssize_t length = 0;
    length = 0;
    auto timeout = -1;
    auto nFdsReady = epoll_wait(m_epollFd, m_epollEvents, MAX_EPOLL_EVENTS, timeout);

    if (nFdsReady == -1) {
        return length;
    }

    for (auto n = 0; n < nFdsReady; ++n) {
        if (m_epollEvents[n].data.fd == m_stopPipeFd[m_pipeReadIdx]) {
            break;
        }

        length = read(m_epollEvents[n].data.fd, eventBuffer.data(), eventBuffer.size());
        if (length == -1) {
            m_error = errno;
            if(m_error == EINTR){
                break;
            }
        }
    }

    return length;
}

void Inotify::readEventsFromBuffer(
    uint8_t* buffer, ssize_t length, std::vector<FileSystemEvent>& events)
{
    ssize_t i = 0;
    while (i < length) {
        inotify_event* event = ((struct inotify_event*)&buffer[i]);

        if(event->mask & IN_IGNORED){
            i += EVENT_SIZE + event->len;
            auto filePath = m_directoryMap[event->wd];
            m_directoryMap.erase(event->wd);
            m_watchMap.erase(filePath);

            continue;
        }

        auto path = wdToPath(event->wd);
        if (event->len) {
        path += "/";
        path += std::string(event->name,event->len);
        }

        // if (isDirectory(path)) {
        //     event->mask |= IN_ISDIR;
        // }
        FileSystemEvent fsEvent(event->wd, event->mask, path, std::chrono::steady_clock::now());

        if (!fsEvent.m_path.empty()) {
            events.push_back(fsEvent);

        } else {
            // Event is not complete --> ignore
        }

        i += EVENT_SIZE + event->len;
    }
}

void Inotify::filterEvents(
    std::vector<FileSystemEvent>& events, std::queue<FileSystemEvent>& eventQueue)
{
    for (auto eventIt = events.begin(); eventIt < events.end();) {
        FileSystemEvent currentEvent = *eventIt;
        if (isOnTimeout(currentEvent.m_eventTime)) {
            eventIt = events.erase(eventIt);
            m_onEventTimeout(currentEvent);
        } else {
            m_lastEventTime = currentEvent.m_eventTime;
            eventQueue.push(currentEvent);
            eventIt++;
        }
    }
}

bool Inotify::isDirectory(const std::string &path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) == 0) {
        return S_ISREG(path_stat.st_mode);
    }
    return false;
}

bool Inotify::exists(const std::string &path) {
    struct stat file_stat;
    if (stat(path.c_str(), &file_stat) == 0) {
        return true;
    }
    return false;
}

}