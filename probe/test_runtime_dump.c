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

// Кадры и контекст раскрутки лежат вне стека: отчёт о его переполнении не
// должен расходовать остаток. Пишет отчёт один поток, первым захвативший
// reportingThread.
static void *stackFrames[8192];
static CONTEXT unwindContext;
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

#if defined(_M_X64) || defined(_M_ARM64)

static DWORD64 ProgramCounter(const CONTEXT *context)
{
#if defined(_M_ARM64)
    return context->Pc;
#else
    return context->Rip;
#endif
}

static DWORD64 StackPointer(const CONTEXT *context)
{
#if defined(_M_ARM64)
    return context->Sp;
#else
    return context->Rsp;
#endif
}

#endif

// Цепочка раскручивается от контекста сбоя, а не от самого отчёта: на ARM64
// обход из фильтра обрывается на диспетчере исключений. Каждый шаг сверяется
// с границами стека; сбой внутри раскрутки испорченного стека завершает
// процесс кодом исходного сбоя (см. ReportException).
static uint32_t CaptureFrames(const CONTEXT *fault)
{
    uint32_t count = 0u;
#if defined(_M_X64) || defined(_M_ARM64)
    ULONG_PTR low = 0u;
    ULONG_PTR high = 0u;
    GetCurrentThreadStackLimits(&low, &high);
    CONTEXT *context = &unwindContext;
    *context = *fault;
    while (count < sizeof(stackFrames) / sizeof(stackFrames[0]))
    {
        const DWORD64 pc = ProgramCounter(context);
        const DWORD64 sp = StackPointer(context);
        stackFrames[count++] = (void *)(uintptr_t)pc;
        if (sp < low || sp > high - sizeof(DWORD64))
            break;
        DWORD64 imageBase = 0u;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &imageBase, NULL);
        if (function != NULL)
        {
#if defined(_M_ARM64)
            // Кадр ARM64 восстанавливается через указатель кадра.
            if (context->Fp < low || context->Fp > high - 2u * sizeof(DWORD64))
                break;
#endif
            void *handlerData = NULL;
            DWORD64 establisherFrame = 0u;
            (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, context,
                                   &handlerData, &establisherFrame, NULL);
        }
        else
        {
            // Без данных раскрутки — лист или переход по негодному адресу:
            // адрес возврата ещё в регистре связи (ARM64) или на вершине
            // стека (x64).
#if defined(_M_ARM64)
            context->Pc = context->Lr;
#else
            context->Rip = *(const DWORD64 *)(uintptr_t)sp;
            context->Rsp = sp + sizeof(DWORD64);
#endif
        }
        // Стек раскручивается только вверх; нулевой адрес возврата — конец
        // цепочки.
        const DWORD64 nextPc = ProgramCounter(context);
        const DWORD64 nextSp = StackPointer(context);
        if (nextPc == 0u || nextSp < sp || (nextSp == sp && nextPc == pc))
            break;
    }
#else
    (void)fault;
#endif
    return count;
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

#if defined(_M_ARM64)
// Probe only: registers and memory near them at the fault.
static void ProbeWriteRegister(const char *name, DWORD64 value)
{
    LaiueTestRuntimeWrite("  ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite(" ");
    WriteHex(value);
    if (value >= 0x10000u && ModuleOf((const void *)(uintptr_t)value) != NULL)
    {
        LaiueTestRuntimeWrite(" (");
        WriteLocation((const void *)(uintptr_t)value);
        LaiueTestRuntimeWrite(")");
    }
    LaiueTestRuntimeWrite("\n");
}

static void ProbeDumpMemory(const char *name, DWORD64 address, uint32_t bytes)
{
    MEMORY_BASIC_INFORMATION info;
    if (address < 0x10000u ||
        VirtualQuery((LPCVOID)(uintptr_t)address, &info, sizeof(info)) == 0u ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0u ||
        (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0u)
        return;
    const DWORD64 start = address & ~(DWORD64)7u;
    const DWORD64 end = (DWORD64)(uintptr_t)info.BaseAddress + info.RegionSize;
    if (end - start < bytes)
        bytes = (uint32_t)(end - start);
    LaiueTestRuntimeWrite("Memory at ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite(" ");
    WriteHex(start);
    LaiueTestRuntimeWrite(":\n");
    const DWORD64 *words = (const DWORD64 *)(uintptr_t)start;
    for (uint32_t index = 0u; index * 8u + 8u <= bytes; ++index)
    {
        LaiueTestRuntimeWrite(index % 4u == 0u ? "  " : " ");
        WriteHex(words[index]);
        if (index % 4u == 3u)
            LaiueTestRuntimeWrite("\n");
    }
    LaiueTestRuntimeWrite("\n");
}

static void ProbeDumpContext(const CONTEXT *context)
{
    static const char *const names[29] = {
        "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",
        "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19",
        "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28"};
    LaiueTestRuntimeWrite("Registers:\n");
    for (uint32_t index = 0u; index < 29u; ++index)
        ProbeWriteRegister(names[index], context->X[index]);
    ProbeWriteRegister("fp", context->Fp);
    ProbeWriteRegister("lr", context->Lr);
    ProbeWriteRegister("sp", context->Sp);
    ProbeWriteRegister("pc", context->Pc);
    ProbeDumpMemory("pc-0x40", context->Pc - 0x40u, 0x80u);
    ProbeDumpMemory("sp", context->Sp, 0x800u);
    for (uint32_t index = 0u; index < 29u; ++index)
    {
        if (index <= 8u || index >= 19u)
            ProbeDumpMemory(names[index], context->X[index], 0x180u);
    }
}
#endif
// Фильтр верхнего уровня вызывается при поиске обработчика, пока стек цел.
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
    WriteFrames(stackFrames, CaptureFrames(pointers->ContextRecord));
#if defined(_M_ARM64)
    ProbeDumpContext(pointers->ContextRecord);
#endif
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

// Тело теста на POSIX — обычный main, поэтому отчёт ставится ещё до него.
__attribute__((constructor)) static void ReportCrashesBeforeMain(void)
{
    LaiueTestRuntimeReportCrashes();
}

#else

void LaiueTestRuntimeReportCrashes(void) {}

#endif
