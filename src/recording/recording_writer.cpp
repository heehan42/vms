#include "recording/recording_writer.h"

#include <pthread.h>
#include <signal.h>

#include <chrono>
#include <utility>


namespace vms::recording {


RecordingWriter::RecordingWriter(RecordingOptions options, RecordedFileCatalog closed_files,
                                 RecordingCatalogListener listener)
    : options_(options), store_(std::move(options), std::move(closed_files), std::move(listener)) {
    // 종료 시그널은 이벤트 루프 스레드가 signalfd로 받는다. 녹화 스레드가 시그널을 받으면 기본 동작으로 프로세스가
    // 끝나므로, 모든 시그널을 막은 상태로 스레드를 만들어 마스크를 물려준 뒤 이 스레드의 마스크를 되돌린다.
    sigset_t all_signals;
    sigset_t previous_signals;
    sigfillset(&all_signals);
    pthread_sigmask(SIG_BLOCK, &all_signals, &previous_signals);
    thread_ = std::thread([this] { Run(); });
    pthread_sigmask(SIG_SETMASK, &previous_signals, nullptr);
}


RecordingWriter::~RecordingWriter() {
    {
        const std::lock_guard lock(mutex_);
        is_stop_requested_ = true;
    }
    work_available_.notify_one();
    thread_.join();
}


void RecordingWriter::Push(RecordingTask task) {
    const std::size_t task_bytes = QueuedBytesOf(task);
    {
        const std::lock_guard lock(mutex_);
        queued_bytes_ += task_bytes;
        queue_.push_back(std::move(task));
    }
    work_available_.notify_one();
}


bool RecordingWriter::HasRoom() const {
    const std::lock_guard lock(mutex_);
    return queued_bytes_ < options_.queue_byte_limit;
}


void RecordingWriter::WaitUntilIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !is_task_running_; });
}


void RecordingWriter::Run() {
    const auto sync_interval = std::chrono::seconds(options_.sync_interval_seconds);
    auto next_sync_time = std::chrono::steady_clock::now() + sync_interval;

    std::unique_lock lock(mutex_);
    while (true) {
        // 작업이 계속 들어와 대기가 시간 초과로 끝나지 않아도 동기화 주기를 지키도록 매번 기한을 확인한다.
        if (std::chrono::steady_clock::now() >= next_sync_time) {
            lock.unlock();
            store_.SyncOpenFiles();
            lock.lock();
            next_sync_time = std::chrono::steady_clock::now() + sync_interval;
            continue;
        }

        // 정지 요청을 받아도 큐가 빌 때까지는 끝내지 않는다. 마지막 CloseFile까지 처리해야 파일이 닫힌다.
        if (queue_.empty()) {
            if (is_stop_requested_) {
                break;
            }
            work_available_.wait_until(lock, next_sync_time);
            continue;
        }

        RecordingTask task = std::move(queue_.front());
        queue_.pop_front();
        is_task_running_ = true;
        lock.unlock();

        // 바이트 합은 쓰기를 마친 뒤 줄인다. 그래서 디스크가 막힌 동안 쓰는 중인 조각도 상한에 든다.
        const std::size_t task_bytes = QueuedBytesOf(task);
        store_.Execute(task);

        lock.lock();
        queued_bytes_ -= task_bytes;
        is_task_running_ = false;
        if (queue_.empty()) {
            idle_.notify_all();
        }
    }
    lock.unlock();

    // 정상 종료에서는 카메라가 끊김을 알려 이미 닫혔지만, 남은 파일이 있으면 여기서 닫는다.
    store_.CloseAllFiles();
}


}  // namespace vms::recording
