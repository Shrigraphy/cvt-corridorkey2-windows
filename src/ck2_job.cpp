// ---------------------------------------------------------------------------
// CorridorKey 2 for Resolve - runs the Python worker and follows its progress
//
// From Creative Video Tips (CVT)  |  https://creativevideotips.com
// Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
// ---------------------------------------------------------------------------
#include "ck2_job.h"
#include "ck2_core.h"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <thread>
#include <vector>

namespace ck2 {

namespace {
// "2 chunks of 80" / "1 chunk of 23"
std::string chunksText(const std::string& chunks, const std::string& window) {
    return chunks + (chunks == "1" ? " chunk of " : " chunks of ") + window;
}

#ifdef _WIN32
std::wstring quoteArg(const std::wstring& value) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        out.push_back(c);
        slashes = 0;
    }
    out.append(2 * slashes, L'\\');
    return out + L"\"";
}
#endif
}  // namespace

std::shared_ptr<Job> Job::start(const std::string& python, const std::string& worker,
                                const std::string& jobPath, const std::string& logPath,
                                Callback onUpdate, std::string& error) {
#ifdef _WIN32
    HANDLE reader = nullptr, writer = nullptr;
    SECURITY_ATTRIBUTES attrs{sizeof(attrs), nullptr, TRUE};
    if (!CreatePipe(&reader, &writer, &attrs, 0)) { error = "Could not create worker pipe"; return nullptr; }
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    const auto program = std::filesystem::u8path(python);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attrs, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE log = CreateFileW(std::filesystem::u8path(logPath).c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &attrs, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE group = CreateJobObjectW(nullptr, nullptr);
    auto releasePipes = [&] {
        CloseHandle(writer);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    };
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (input == INVALID_HANDLE_VALUE || log == INVALID_HANDLE_VALUE || !group ||
        !SetInformationJobObject(group, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = "Could not prepare the worker process (" + std::to_string(GetLastError()) + ")";
        releasePipes();
        CloseHandle(reader);
        if (group) CloseHandle(group);
        return nullptr;
    }
    HANDLE inherit[] = {input, writer, log};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    const BOOL initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes);
    PROCESS_INFORMATION child{};
    BOOL started = FALSE;
    if (initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                  inherit, sizeof(inherit), nullptr, nullptr)) {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = writer;
        startup.StartupInfo.hStdError = log;
        startup.lpAttributeList = attributes;
        std::wstring command = quoteArg(program.native()) + L" -E -s -X utf8 -u " +
            quoteArg(std::filesystem::u8path(worker).native()) + L" run " +
            quoteArg(std::filesystem::u8path(jobPath).native());
        started = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
            &startup.StartupInfo, &child);
    }
    const DWORD startError = GetLastError();
    if (initialized) DeleteProcThreadAttributeList(attributes);
    releasePipes();
    if (!started || !AssignProcessToJobObject(group, child.hProcess) || ResumeThread(child.hThread) == DWORD(-1)) {
        const DWORD code = started ? GetLastError() : startError;
        if (started) {
            TerminateProcess(child.hProcess, 1);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
        CloseHandle(group);
        CloseHandle(reader);
        error = "Could not start CorridorKey Python (" + std::to_string(code) + ")";
        return nullptr;
    }
    CloseHandle(child.hThread);
    auto job = std::shared_ptr<Job>(new Job());
    job->callback_ = std::move(onUpdate);
    job->process_ = child.hProcess;
    job->processGroup_ = group;
    job->running_ = true;
    std::thread([job, reader] { job->readLoop(reader); }).detach();
#else
    int fds[2];
    if (pipe(fds) != 0) { error = "pipe failed"; return nullptr; }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, logPath.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    std::vector<std::string> args = {python, "-u", worker, "run", jobPath};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // A small, clean environment: the host's own Python settings must not leak in.
    const char* home = std::getenv("HOME");
    std::vector<std::string> env = {
        std::string("HOME=") + (home ? home : ""),
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "LANG=en_US.UTF-8",
        "PYTORCH_ENABLE_MPS_FALLBACK=1",
        "PYTHONNOUSERSITE=1",
    };
    if (const char* tmp = std::getenv("TMPDIR")) env.push_back(std::string("TMPDIR=") + tmp);
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, python.c_str(), &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        error = std::string("could not start Python: ") + std::strerror(rc);
        return nullptr;
    }

    auto job = std::shared_ptr<Job>(new Job());
    job->callback_ = std::move(onUpdate);
    job->pid_ = pid;
    job->running_ = true;
    std::thread([job, fd = fds[0]] { job->readLoop(fd); }).detach();
#endif
    return job;
}

bool Job::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void Job::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
    if (running_) {
        cancelled_ = true;
        TerminateJobObject(processGroup_, 1);
    }
#else
    if (running_ && pid_ > 0) {
        cancelled_ = true;
        kill(pid_, SIGTERM);
    }
#endif
}

void Job::detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
}

void Job::report(const JobUpdate& u) {
    // Held during the call so detach() waits until no callback is running.
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback_) callback_(u);
}

#ifdef _WIN32
void Job::readLoop(void* pipe) {
#else
void Job::readLoop(int fd) {
#endif
    std::string pending, lastError, doneText;
    char buf[4096];
    while (true) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &n, nullptr) || n == 0) break;
#else
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
#endif
        pending.append(buf, static_cast<size_t>(n));
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string ev = jsonField(line, "event");
            JobUpdate u;
            if (ev == "plan") {
                u.status = "Starting: " + jsonField(line, "frames") + " frames in " +
                           chunksText(jsonField(line, "chunks"), jsonField(line, "window"));
            } else if (ev == "status") {
                std::string m = jsonField(line, "message");
                u.status = m.empty() ? "" : std::string(1, static_cast<char>(std::toupper(m[0]))) + m.substr(1);
            } else if (ev == "progress") {
                u.status = "Keying frame " + jsonField(line, "done") + " of " + jsonField(line, "total") +
                           " (chunk " + jsonField(line, "chunk") + " of " + jsonField(line, "chunks") + ")";
            } else if (ev == "done") {
                doneText = "Done: " + jsonField(line, "frames") + " frames in " +
                           jsonField(line, "seconds") + " s (" +
                           chunksText(jsonField(line, "chunks"), jsonField(line, "window")) + " frames)";
                continue;
            } else if (ev == "error") {
                lastError = jsonField(line, "message");
                continue;
            }
            if (!u.status.empty()) report(u);
        }
    }
#ifdef _WIN32
    CloseHandle(pipe);
    DWORD st = 1;
    WaitForSingleObject(process_, INFINITE);
    GetExitCodeProcess(process_, &st);
#else
    close(fd);
    int st = 0;
    while (waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
#endif
    bool cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        cancelled = cancelled_;
#ifdef _WIN32
        // The progress window's cancel terminates Python with SIGTERM's numeric code.
        cancelled = cancelled || st == SIGTERM;
        // Closing the job drops any child that outlived the worker.
        CloseHandle(processGroup_);
        CloseHandle(process_);
#endif
    }
    JobUpdate u;
    u.finished = true;
#ifdef _WIN32
    u.ok = st == 0 && !doneText.empty();
#else
    u.ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && !doneText.empty();
#endif
    if (u.ok) u.status = doneText;
    else if (cancelled || lastError == "Cancelled") u.status = "Stopped";
    else if (!lastError.empty()) u.status = "Error: " + lastError;
    else u.status = "Error: the worker stopped (see the log)";
    report(u);
}

}  // namespace ck2
