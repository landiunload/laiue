// Отчёт о сбое теста. Без него редкое падение в CI оставляет в журнале одно
// слово вроде «SegFault», за которым на Windows стоит любой из десятка кодов:
// нарушение доступа, ошибка подкачки страницы, переполнение стека. Отчёт
// печатает код, адрес и цепочку вызовов «модуль+смещение»; смещение находит
// функцию в сборке того же коммита. Процесс затем завершается так же, как без
// отчёта, и CTest видит прежний итог.
//
// Отдельная единица трансляции: сигналам POSIX нужны макросы возможностей до
// первого системного заголовка, а тесты включают их раньше test_runtime.h.
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#endif

#include "test_runtime.h"

#include <stddef.h>

// Санитайзеры ставят свои обработчики и пишут отчёт подробнее этого.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LAIUE_TEST_RUNTIME_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define LAIUE_TEST_RUNTIME_SANITIZED 1
#endif
#endif

#if !defined(LAIUE_TEST_RUNTIME_SANITIZED)
#if defined(_WIN32)
#define LAIUE_TEST_RUNTIME_WINDOWS 1
#elif defined(__unix__) || defined(__APPLE__)
#define LAIUE_TEST_RUNTIME_POSIX 1
#endif
#endif

#if defined(LAIUE_TEST_RUNTIME_WINDOWS) || defined(LAIUE_TEST_RUNTIME_POSIX)

// "0x" и цифры без ведущих нулей.
static const char *FormatHex(uint64_t value, char text[19])
{
    static const char digits[] = "0123456789abcdef";
    uint32_t length = 0u;
    uint32_t shift = 60u;
    text[length++] = '0';
    text[length++] = 'x';
    while (shift != 0u && ((value >> shift) & 0xFu) == 0u)
        shift -= 4u;
    for (;;)
    {
        text[length++] = digits[(value >> shift) & 0xFu];
        if (shift == 0u)
            break;
        shift -= 4u;
    }
    text[length] = '\0';
    return text;
}

#endif

#if defined(LAIUE_TEST_RUNTIME_WINDOWS)

// Кадры лежат вне стека: отчёт о его переполнении не должен расходовать
// остаток. Пишет отчёт один поток, первым захвативший reportingThread.
static void *stackFrames[8192];
static volatile LONG reportingThread;
static DWORD reportedCode;

static void WriteHex(uint64_t value)
{
    char text[19];
    LaiueTestRuntimeWrite(FormatHex(value, text));
}

static void WriteDecimal(uint32_t value)
{
    char text[11];
    uint32_t length = sizeof(text) - 1u;
    text[length] = '\0';
    do
    {
        text[--length] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    LaiueTestRuntimeWrite(text + length);
}

static HMODULE ModuleOf(const void *address)
{
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module))
        return NULL;
    return module;
}

// «модуль+смещение»; адрес вне модулей печатается как есть. Имя модуля
// переписывается в ASCII в буферах на стеке, без кучи: сбой мог её испортить.
static void WriteLocation(const void *address)
{
    const HMODULE module = ModuleOf(address);
    WCHAR path[MAX_PATH];
    const DWORD length = module != NULL ? GetModuleFileNameW(module, path, MAX_PATH) : 0u;
    if (length == 0u || length >= MAX_PATH)
    {
        WriteHex((uint64_t)(uintptr_t)address);
        return;
    }
    DWORD start = 0u;
    for (DWORD index = 0u; index < length; ++index)
    {
        if (path[index] == L'\\' || path[index] == L'/')
            start = index + 1u;
    }
    char name[MAX_PATH];
    DWORD size = 0u;
    for (DWORD index = start; index < length; ++index)
        name[size++] = path[index] < 0x80u ? (char)path[index] : '?';
    name[size] = '\0';
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("+");
    WriteHex((uint64_t)((uintptr_t)address - (uintptr_t)module));
}

static const char *ExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return " (access violation)";
    case EXCEPTION_IN_PAGE_ERROR:
        return " (in-page error)";
    case EXCEPTION_STACK_OVERFLOW:
        return " (stack overflow)";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return " (misaligned data)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return " (illegal instruction)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return " (integer division by zero)";
    case EXCEPTION_BREAKPOINT:
        return " (breakpoint)";
    default:
        return "";
    }
}

// Адрес возврата при переходе по негодному указателю на функцию: вызов уже
// положил его на вершину стека (x64) или в регистр связи (ARM64).
static void *ReturnAddress(const CONTEXT *context)
{
#if defined(_M_ARM64)
    return (void *)(uintptr_t)context->Lr;
#elif defined(_M_X64)
    ULONG_PTR low = 0u;
    ULONG_PTR high = 0u;
    GetCurrentThreadStackLimits(&low, &high);
    if (context->Rsp < low || context->Rsp > high - sizeof(void *))
        return NULL;
    return *(void *const *)(uintptr_t)context->Rsp;
#else
    (void)context;
    return NULL;
#endif
}

// Подряд идущие повторы (рекурсия) сжимаются в одну строку; из длинной
// цепочки печатаются начало и конец.
static void WriteFrames(void *const *frames, uint32_t count)
{
    uint32_t lines = 0u;
    for (uint32_t index = 0u; index < count; ++index)
        lines += index == 0u || frames[index] != frames[index - 1u] ? 1u : 0u;
    uint32_t line = 0u;
    for (uint32_t index = 0u; index < count; ++line)
    {
        uint32_t next = index + 1u;
        while (next < count && frames[next] == frames[index])
            ++next;
        if (line < 48u || line + 16u >= lines)
        {
            LaiueTestRuntimeWrite("  ");
            WriteLocation(frames[index]);
            if (next - index > 1u)
            {
                LaiueTestRuntimeWrite(" (repeated ");
                WriteDecimal(next - index);
                LaiueTestRuntimeWrite(" times)");
            }
            LaiueTestRuntimeWrite("\n");
        }
        else if (line == 48u)
        {
            LaiueTestRuntimeWrite("  ...\n");
        }
        index = next;
    }
}

// Фильтр верхнего уровня вызывается при поиске обработчика, пока стек цел:
// обход от фильтра проходит через диспетчер исключений в упавшую функцию и
// дальше к её вызывающим. RtlCaptureStackBackTrace сверяет кадры с границами
// стека и на испорченном стеке просто останавливается.
static LONG WINAPI ReportException(EXCEPTION_POINTERS *pointers)
{
    const LONG thread = (LONG)GetCurrentThreadId();
    const LONG owner = InterlockedCompareExchange(&reportingThread, thread, 0);
    // Сбой внутри самого отчёта: процесс завершается кодом исходного сбоя.
    if (owner == thread)
        TerminateProcess(GetCurrentProcess(), reportedCode);
    // Отчёт уже пишет другой поток, и его сбой завершит процесс.
    if (owner != 0)
        Sleep(INFINITE);

    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    reportedCode = record->ExceptionCode;
    LaiueTestRuntimeWrite("\nUnhandled exception ");
    WriteHex(record->ExceptionCode);
    LaiueTestRuntimeWrite(ExceptionName(record->ExceptionCode));
    LaiueTestRuntimeWrite(" at ");
    WriteLocation(record->ExceptionAddress);
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2u)
    {
        const ULONG_PTR access = record->ExceptionInformation[0];
        LaiueTestRuntimeWrite(access == 0u   ? ", reading "
                              : access == 1u ? ", writing "
                                             : ", executing ");
        WriteHex(record->ExceptionInformation[1]);
        if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && record->NumberParameters >= 3u)
        {
            LaiueTestRuntimeWrite(", status ");
            WriteHex(record->ExceptionInformation[2]);
        }
    }
    LaiueTestRuntimeWrite("\nStack:\n");

    if (ModuleOf(record->ExceptionAddress) == NULL)
    {
        // Переход по негодному указателю на функцию: от адреса вне модулей
        // стек не раскрутить, но вызвавшего видно по адресу возврата.
        void *const frames[2] = {record->ExceptionAddress, ReturnAddress(pointers->ContextRecord)};
        WriteFrames(frames, frames[1] != NULL ? 2u : 1u);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint32_t count = RtlCaptureStackBackTrace(
        0u, (DWORD)(sizeof(stackFrames) / sizeof(stackFrames[0])), stackFrames, NULL);
    // Первые кадры принадлежат самому отчёту и диспетчеру исключений.
    uint32_t first = 0u;
    while (first < count && stackFrames[first] != record->ExceptionAddress)
        ++first;
    if (first == count)
        first = 0u;
    WriteFrames(stackFrames + first, count - first);
    return EXCEPTION_CONTINUE_SEARCH;
}

void LaiueTestRuntimeReportCrashes(void)
{
    // Переполнению стека нужен запас, на котором отчёт успеет выполниться.
    ULONG guarantee = 32u * 1024u;
    (void)SetThreadStackGuarantee(&guarantee);
    (void)SetUnhandledExceptionFilter(ReportException);
}

#elif defined(LAIUE_TEST_RUNTIME_POSIX)

#include <signal.h>
#include <string.h>
#include <unistd.h>

#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define LAIUE_TEST_RUNTIME_BACKTRACE 1
#endif

// Запасной стек: на нём обработчик успевает отчитаться и о переполнении
// основного.
static char alternateStack[64u * 1024u];

// Внутри обработчика сигнала — только write(2): stdio там небезопасна.
static void WriteSafe(const char *text)
{
    size_t length = 0u;
    while (text[length] != '\0')
        ++length;
    while (length != 0u)
    {
        const ssize_t written = write(STDOUT_FILENO, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= (size_t)written;
    }
}

static const char *SignalName(int number)
{
    switch (number)
    {
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
    case SIGTRAP:
        return "SIGTRAP";
    default:
        return "signal";
    }
}

static void ReportSignal(int number, siginfo_t *info, void *context)
{
    (void)context;
    WriteSafe("\nFatal ");
    WriteSafe(SignalName(number));
    if (number != SIGABRT && info != NULL)
    {
        char text[19];
        WriteSafe(", address ");
        WriteSafe(FormatHex((uint64_t)(uintptr_t)info->si_addr, text));
    }
    WriteSafe("\n");
#if defined(LAIUE_TEST_RUNTIME_BACKTRACE)
    void *frames[64];
    const int count = backtrace(frames, 64);
    WriteSafe("Stack:\n");
    backtrace_symbols_fd(frames, count, STDOUT_FILENO);
#endif
    // Обработчик снят при входе, а сигнал не заблокирован: повтор завершает
    // процесс тем же сигналом, что и без отчёта.
    (void)raise(number);
}

void LaiueTestRuntimeReportCrashes(void)
{
    stack_t stack;
    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof(alternateStack);
    (void)sigaltstack(&stack, NULL);
#if defined(LAIUE_TEST_RUNTIME_BACKTRACE)
    // Первый вызов подгружает раскрутчик стека; в обработчике сигнала это
    // было бы небезопасно.
    void *warmup[2];
    (void)backtrace(warmup, 2);
#endif
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = ReportSignal;
    // SA_RESETHAND в Linux равен 0x80000000u, а поле знаковое.
    action.sa_flags = (int)(SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER);
    (void)sigemptyset(&action.sa_mask);
    static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
    for (size_t index = 0u; index < sizeof(signals) / sizeof(signals[0]); ++index)
        (void)sigaction(signals[index], &action, NULL);
}

#else

void LaiueTestRuntimeReportCrashes(void) {}

#endif
