#include "Crash.h"

#include "Host.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <typeinfo>

#ifdef _WIN32
#include <windows.h>

#include <csignal>
#include <dbghelp.h>
#include <io.h>
#else
#include <cerrno>
#include <csignal>
#include <execinfo.h>
#include <pthread.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#endif

namespace openhorizon {

namespace {

/// Matches the width of logLine's time column, which a crash line cannot
/// fill: reading the clock and formatting a double are not safe in a
/// signal handler.
constexpr std::string_view kCrashPrefix = "[    crash] fatal   ";
constexpr std::size_t kMaxDetail = 1000;
constexpr int kMaxFrames = 64;

/// Cuts `text` to at most `limit` bytes without splitting a UTF-8
/// sequence.
std::string_view clipUtf8(std::string_view text, std::size_t limit)
{
    if (text.size() <= limit) {
        return text;
    }
    std::size_t size = limit;
    while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) {
        --size;
    }
    return text.substr(0, size);
}

enum class CrashEntry { First, Nested };

#ifdef _WIN32

constexpr DWORD kMsvcCppException = 0xE06D7363;
constexpr DWORD kHeapCorruption = 0xC0000374;
constexpr int kAbortExitCode = 3;

std::atomic<DWORD> g_crashingThread{0};
std::wstring g_reporterPath;
std::wstring g_reporterCommandLine;
std::wstring g_dumpPath;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
bool g_installed = false;

std::wstring widenUtf8(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

/// The first thread to crash reports; a crash inside its own report ends
/// the process at once, and any other thread waits for the report to end
/// it.
CrashEntry enterCrash()
{
    const DWORD self = GetCurrentThreadId();
    DWORD expected = 0;
    if (g_crashingThread.compare_exchange_strong(expected, self)) {
        return CrashEntry::First;
    }
    if (expected == self) {
        return CrashEntry::Nested;
    }
    for (;;) {
        Sleep(INFINITE);
    }
}

void writeHandle(HANDLE handle, std::string_view text)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    while (!text.empty()) {
        DWORD written = 0;
        if (!WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written == 0) {
            return;
        }
        text.remove_prefix(written);
    }
}

HANDLE logHandle()
{
    const int descriptor = logFileDescriptor();
    return descriptor >= 0 ? reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)) : INVALID_HANDLE_VALUE;
}

void writeCrashText(std::string_view text)
{
    writeHandle(logHandle(), text);
    writeHandle(GetStdHandle(STD_ERROR_HANDLE), text);
}

void writeCrashLine(std::string_view text)
{
    writeCrashText(kCrashPrefix);
    writeCrashText(text);
    writeCrashText("\n");
}

const char* exceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case kHeapCorruption:
        return "STATUS_HEAP_CORRUPTION";
    case kMsvcCppException:
        return "uncaught C++ exception";
    default:
        return "unknown exception";
    }
}

/// Walks the stack from `context` and writes one line per frame: module,
/// symbol and source line when the PDB is found, the address otherwise.
void writeStack(CONTEXT context)
{
    writeCrashLine("stack:");
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const bool symbols = SymInitialize(process, nullptr, TRUE) != FALSE;

    STACKFRAME64 frame{};
#if defined(_M_X64)
    constexpr DWORD kMachine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
#elif defined(_M_ARM64)
    constexpr DWORD kMachine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrStack.Offset = context.Sp;
    frame.AddrFrame.Offset = context.Fp;
#else
#error "writeStack needs the registers of this architecture"
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;

    // Static rather than on a stack that may just have overflowed; the
    // crash guard lets only one thread in here.
    alignas(SYMBOL_INFO) static char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    for (int index = 0; index < kMaxFrames; ++index) {
        if (!StackWalk64(kMachine, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                SymGetModuleBase64, nullptr)) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        if (address == 0) {
            break;
        }
        char text[1024];
        int length
            = std::snprintf(text, sizeof(text), "  #%-2d 0x%016llx", index, static_cast<unsigned long long>(address));
        const auto append = [&](const char* format, auto... values) {
            if (length >= 0 && static_cast<std::size_t>(length) < sizeof(text)) {
                const int more
                    = std::snprintf(text + length, sizeof(text) - static_cast<std::size_t>(length), format, values...);
                length = more < 0 ? length : length + more;
            }
        };
        if (symbols) {
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            if (SymGetModuleInfo64(process, address, &module)) {
                append(" %s!", module.ModuleName);
            }
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
            std::memset(symbolBuffer, 0, sizeof(symbolBuffer));
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol)) {
                append("%s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
            }
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line)) {
                append(" (%s:%lu)", line.FileName, static_cast<unsigned long>(line.LineNumber));
            }
        }
        append("%s", "\n");
        if (length > 0) {
            writeCrashText(std::string_view(text, std::min(static_cast<std::size_t>(length), sizeof(text) - 1)));
        }
    }
    if (symbols) {
        SymCleanup(process);
    }
}

void writeMinidump(EXCEPTION_POINTERS* pointers)
{
    if (g_dumpPath.empty()) {
        return;
    }
    const HANDLE file
        = CreateFileW(g_dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        writeCrashLine("cannot create the minidump file");
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = pointers;
    exception.ClientPointers = FALSE;
    // Thread and module lists plus the memory the stacks point at: enough
    // for a debugger to show every thread's stack and locals, a few
    // megabytes rather than the whole heap.
    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL written = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file, type, pointers ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
    writeCrashLine(
        written ? "minidump written next to the log (openhorizon.dmp)" : "the minidump could not be written");
}

void launchReporter(std::string_view detail)
{
    if (g_reporterPath.empty()) {
        return;
    }
    std::wstring commandLine = g_reporterCommandLine;
    if (!detail.empty()) {
        commandLine += L' ';
        commandLine += widenUtf8(
            windowsCommandLineArgument(std::string(kCrashDetailArgument) + std::string(clipUtf8(detail, kMaxDetail))));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(g_reporterPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
            &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

[[noreturn]] void endAfterAbort()
{
    std::_Exit(kAbortExitCode);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* pointers)
{
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    if (record->ExceptionCode == kMsvcCppException && g_previousFilter) {
        // The C++ runtime's own filter makes the exception current and
        // calls std::terminate, where onTerminate can name it. It does not
        // return; if a filter set by someone else does, the exception is
        // reported below without its message.
        static_cast<void>(g_previousFilter(pointers));
    }
    if (enterCrash() == CrashEntry::Nested) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    tryFlushLogFile();
    char text[256];
    int length = std::snprintf(text, sizeof(text), "exception 0x%08lx (%s) at 0x%016llx",
        static_cast<unsigned long>(record->ExceptionCode), exceptionName(record->ExceptionCode),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(record->ExceptionAddress)));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && length > 0
        && static_cast<std::size_t>(length) < sizeof(text)) {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        const char* access = kind == 0 ? "reading" : kind == 1 ? "writing" : "executing";
        std::snprintf(text + length, sizeof(text) - static_cast<std::size_t>(length), ", %s address 0x%016llx", access,
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    writeCrashLine(text);
    writeStack(*pointers->ContextRecord);
    writeMinidump(pointers);
    launchReporter({});
    return EXCEPTION_EXECUTE_HANDLER;
}

void writeStackHere()
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    writeStack(context);
}

void __cdecl onAbortSignal(int)
{
    if (enterCrash() == CrashEntry::Nested) {
        endAfterAbort();
    }
    tryFlushLogFile();
    writeCrashLine("abort was called");
    writeStackHere();
    writeMinidump(nullptr);
    launchReporter({});
    endAfterAbort();
}

#else

/// A line put together without allocating, for the signal handler.
class FixedLine {
public:
    FixedLine& text(std::string_view part)
    {
        const std::size_t room = sizeof(m_text) - m_size;
        const std::size_t count = std::min(part.size(), room);
        std::memcpy(m_text + m_size, part.data(), count);
        m_size += count;
        return *this;
    }

    FixedLine& decimal(long long value)
    {
        char digits[24];
        std::size_t count = 0;
        const bool negative = value < 0;
        unsigned long long magnitude
            = negative ? 0ULL - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
        do {
            digits[count++] = static_cast<char>('0' + magnitude % 10U);
            magnitude /= 10U;
        } while (magnitude != 0U);
        if (negative) {
            digits[count++] = '-';
        }
        std::reverse(digits, digits + count);
        return text(std::string_view(digits, count));
    }

    FixedLine& hex(std::uint64_t value)
    {
        char digits[18] = {'0', 'x'};
        std::size_t count = 2;
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const unsigned nibble = static_cast<unsigned>(value >> static_cast<unsigned>(shift)) & 0xFU;
            if (nibble != 0U || started || shift == 0) {
                digits[count++] = "0123456789abcdef"[nibble];
                started = true;
            }
        }
        return text(std::string_view(digits, count));
    }

    std::string_view view() const { return {m_text, m_size}; }

private:
    char m_text[1024] = {};
    std::size_t m_size = 0;
};

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr std::size_t kMaxArgument = 4200;

std::atomic<pid_t> g_crashingThread{0};
// Written at install time (and the detail by the terminate handler after
// it owns the crash), read by the signal handler, which may not allocate.
char g_reporterPath[kMaxArgument] = {};
char g_reportArgument[kMaxArgument] = {};
char g_detailArgument[kMaxDetail + 32] = {};
bool g_hasReporter = false;
bool g_installed = false;

bool copyArgument(char* target, std::size_t capacity, std::string_view prefix, std::string_view value)
{
    if (prefix.size() + value.size() + 1 > capacity) {
        target[0] = '\0';
        return false;
    }
    std::memcpy(target, prefix.data(), prefix.size());
    std::memcpy(target + prefix.size(), value.data(), value.size());
    target[prefix.size() + value.size()] = '\0';
    return true;
}

CrashEntry enterCrash()
{
    const pid_t self = ::gettid();
    pid_t expected = 0;
    if (g_crashingThread.compare_exchange_strong(expected, self)) {
        return CrashEntry::First;
    }
    if (expected == self) {
        return CrashEntry::Nested;
    }
    for (;;) {
        ::pause();
    }
}

void writeDescriptor(int descriptor, std::string_view text)
{
    if (descriptor < 0) {
        return;
    }
    while (!text.empty()) {
        const ssize_t written = ::write(descriptor, text.data(), text.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
}

void writeCrashText(std::string_view text)
{
    writeDescriptor(logFileDescriptor(), text);
    writeDescriptor(STDERR_FILENO, text);
}

void writeCrashLine(std::string_view text)
{
    writeCrashText(kCrashPrefix);
    writeCrashText(text);
    writeCrashText("\n");
}

void writeStackHere()
{
    writeCrashLine("backtrace:");
    void* frames[kMaxFrames];
    const int count = ::backtrace(frames, kMaxFrames);
    const int log = logFileDescriptor();
    if (log >= 0) {
        ::backtrace_symbols_fd(frames, count, log);
    }
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

const char* signalName(int signal)
{
    switch (signal) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

void launchReporter(std::string_view detail)
{
    if (!g_hasReporter) {
        return;
    }
    const bool withDetail = !detail.empty()
        && copyArgument(g_detailArgument, sizeof(g_detailArgument), kCrashDetailArgument, clipUtf8(detail, kMaxDetail));
    // _Fork rather than fork: it runs no atfork handlers and is
    // async-signal-safe, where fork may take the allocator's locks.
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    const pid_t child = ::_Fork();
#else
    const pid_t child = ::fork();
#endif
    if (child != 0) {
        return;
    }
    // The handler's signal is blocked here, and exec keeps the mask.
    sigset_t none;
    sigemptyset(&none);
    ::pthread_sigmask(SIG_SETMASK, &none, nullptr);
    char* arguments[] = {g_reporterPath, g_reportArgument, withDetail ? g_detailArgument : nullptr, nullptr};
    ::execve(g_reporterPath, arguments, environ);
    ::_exit(127);
}

/// Restores the default action for `signal` and raises it again, so the
/// process ends as the signal would have ended it: that exit status, and a
/// core dump where the system keeps them.
[[noreturn]] void dieWithSignal(int signal)
{
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    ::sigaction(signal, &action, nullptr);
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, signal);
    ::pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    ::raise(signal);
    ::_exit(128 + signal);
}

[[noreturn]] void endAfterAbort()
{
    dieWithSignal(SIGABRT);
}

void onFatalSignal(int signal, siginfo_t* info, void* context)
{
    if (enterCrash() == CrashEntry::Nested) {
        dieWithSignal(signal);
    }
    FixedLine line;
    line.text("signal ").decimal(signal).text(" (").text(signalName(signal)).text(")");
    // A positive code means the kernel raised the signal for a fault, so
    // si_addr is the address; kill() and raise() leave the sender there.
    if (info && info->si_code > 0 && signal != SIGABRT) {
        line.text(", code ").decimal(info->si_code);
        line.text(", address ").hex(reinterpret_cast<std::uintptr_t>(info->si_addr));
    }
#if defined(__x86_64__)
    if (context) {
        line.text(", instruction ")
            .hex(static_cast<std::uint64_t>(static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]));
    }
#elif defined(__aarch64__)
    if (context) {
        line.text(", instruction ").hex(static_cast<const ucontext_t*>(context)->uc_mcontext.pc);
    }
#else
    static_cast<void>(context);
#endif
    writeCrashLine(line.view());
    writeStackHere();
    launchReporter({});
    dieWithSignal(signal);
}

/// Where the handler runs, so it still can when the crash is a stack
/// overflow. Well above SIGSTKSZ: the handler puts a FixedLine and the
/// backtrace frames on it.
alignas(16) char g_alternateStack[128U * 1024U];

void installAlternateStack()
{
    stack_t stack{};
    stack.ss_sp = g_alternateStack;
    stack.ss_size = sizeof(g_alternateStack);
    stack.ss_flags = 0;
    ::sigaltstack(&stack, nullptr);
}

#endif

std::string typeName(const std::type_info& type)
{
#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string name = demangled;
        std::free(demangled);
        return name;
    }
    std::free(demangled);
    return type.name();
#else
    // MSVC spells names as "class openhorizon::DataError".
    std::string_view name = type.name();
    for (const std::string_view keyword : {"class ", "struct ", "union ", "enum "}) {
        if (name.starts_with(keyword)) {
            name.remove_prefix(keyword.size());
            break;
        }
    }
    return std::string(name);
#endif
}

[[noreturn]] void onTerminate()
{
    if (enterCrash() == CrashEntry::Nested) {
        endAfterAbort();
    }
    tryFlushLogFile();
    std::string_view headline = "std::terminate was called with no exception in flight";
    std::string described;
    std::string detail;
    try {
        const ExceptionDescription description = describeException(std::current_exception());
        if (!description.type.empty()) {
            described = std::format("uncaught exception {}: {}", description.type, description.message);
            detail = description.message;
            headline = described;
        }
    } catch (...) {
        // Out of memory, most likely; this line needs no allocation.
        headline = "uncaught exception, which could not be described";
    }
    writeCrashLine(headline);
    writeStackHere();
#ifdef _WIN32
    writeMinidump(nullptr);
#endif
    launchReporter(detail);
    endAfterAbort();
}

} // namespace

void installCrashHandlers(const CrashHandlerOptions& options)
{
#ifdef _WIN32
    g_reporterPath = options.reporter.native();
    g_reporterCommandLine.clear();
    if (!g_reporterPath.empty()) {
        g_reporterCommandLine = widenUtf8(windowsCommandLineArgument(pathToUtf8(options.reporter))) + L' '
            + widenUtf8(windowsCommandLineArgument(std::string(kCrashReportArgument) + pathToUtf8(options.logPath)));
    }
    g_dumpPath = options.minidumpPath.native();
    if (!g_installed) {
        g_installed = true;
        const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(onUnhandledException);
        if (previous != onUnhandledException) {
            g_previousFilter = previous;
        }
        std::signal(SIGABRT, onAbortSignal);
        // No "abort() has been called" box and no Windows Error Reporting:
        // the crash report above replaces both.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        // Room for the filter to run after a stack overflow on this thread.
        ULONG guarantee = 64U * 1024U;
        SetThreadStackGuarantee(&guarantee);
        std::set_terminate(onTerminate);
    }
#else
    const std::string& reporter = options.reporter.native();
    g_hasReporter = !reporter.empty() && copyArgument(g_reporterPath, sizeof(g_reporterPath), {}, reporter)
        && copyArgument(g_reportArgument, sizeof(g_reportArgument), kCrashReportArgument, options.logPath.native());
    if (!g_installed) {
        g_installed = true;
        installAlternateStack();
        // backtrace loads libgcc on its first call, which may allocate:
        // done now, so the call in the handler does not.
        void* warmUp[1];
        static_cast<void>(::backtrace(warmUp, 1));
        struct sigaction action{};
        action.sa_sigaction = onFatalSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (const int signal : kFatalSignals) {
            ::sigaction(signal, &action, nullptr);
        }
        std::set_terminate(onTerminate);
    }
#endif
}

std::filesystem::path crashDumpPath(const std::filesystem::path& logPath)
{
    if (logPath.empty()) {
        return {};
    }
    std::filesystem::path path = logPath;
    path.replace_filename("openhorizon.dmp");
    return path;
}

bool errorDialogsAllowed()
{
    if (environmentVariable(kNoCrashDialogVariable)) {
        return false;
    }
    if (const std::optional<std::string> drivers = environmentVariable("SDL_VIDEO_DRIVER")) {
        std::string_view list = *drivers;
        while (!list.empty()) {
            const std::size_t comma = list.find(',');
            const std::string_view driver = list.substr(0, comma);
            if (driver == "offscreen" || driver == "dummy") {
                return false;
            }
            list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
        }
    }
#ifndef _WIN32
    if (!environmentVariable("DISPLAY") && !environmentVariable("WAYLAND_DISPLAY")) {
        return false;
    }
#endif
    return true;
}

std::optional<CrashReport> parseCrashReportArguments(std::span<const std::string> arguments)
{
    if (arguments.size() < 2 || !arguments[1].starts_with(kCrashReportArgument)) {
        return std::nullopt;
    }
    CrashReport report;
    report.logPath = pathFromUtf8(std::string_view(arguments[1]).substr(kCrashReportArgument.size()));
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        if (arguments[i].starts_with(kCrashDetailArgument)) {
            report.detail = arguments[i].substr(kCrashDetailArgument.size());
        }
    }
    return report;
}

std::string crashDialogText(const CrashReport& report, const std::filesystem::path& dumpPath)
{
    std::string text = "OpenHorizon has stopped unexpectedly.\n\n";
    text += report.detail.empty() ? "It ran into a problem it could not recover from." : report.detail;
    if (report.logPath.empty()) {
        text += "\n\nNo log file could be written for this run.";
        return text;
    }
    text += std::format("\n\nThe log file records what happened:\n{}", pathToUtf8(report.logPath));
    std::error_code error;
    const bool withDump = !dumpPath.empty() && std::filesystem::is_regular_file(dumpPath, error);
    if (withDump) {
        text += std::format("\n\nA crash dump was saved next to it:\n{}", pathToUtf8(dumpPath));
    }
    text += withDump ? "\n\nPlease attach both files to your bug report." : "\n\nPlease attach it to your bug report.";
    return text;
}

std::string gameDataErrorText(std::string_view detail, const std::filesystem::path& logPath)
{
    std::string text = std::format("OpenHorizon has stopped because it could not read one of the game's files.\n\n"
                                   "{}\n\n"
                                   "Check that the game folder holds a complete copy of the disc.",
        detail);
    if (!logPath.empty()) {
        text += std::format("\n\nThe log file records what happened:\n{}", pathToUtf8(logPath));
    }
    return text;
}

std::string startupErrorText(std::string_view problem, std::string_view fix)
{
    return std::format("OpenHorizon could not start.\n\n{}\n\n{}", problem, fix);
}

ExceptionDescription describeException(const std::exception_ptr& error)
{
    if (!error) {
        return {};
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return {typeName(typeid(e)), e.what()};
    } catch (...) {
#if defined(__GNUC__) || defined(__clang__)
        if (const std::type_info* type = abi::__cxa_current_exception_type()) {
            return {typeName(*type), {}};
        }
#endif
        return {"unknown type", {}};
    }
}

std::string windowsCommandLineArgument(std::string_view argument)
{
    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // Each backslash before a quote is doubled, and the quote gets
            // one of its own.
            quoted.append(backslashes * 2 + 1, '\\');
        } else {
            quoted.append(backslashes, '\\');
        }
        quoted += c;
        backslashes = 0;
    }
    // Doubled before the closing quote, so none of them escapes it.
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

} // namespace openhorizon
