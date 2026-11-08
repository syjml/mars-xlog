#include <mars/xlog/xloggerbase.h>

#include <unistd.h>

#if defined(__APPLE__)
#include <pthread.h>

namespace {
const pthread_t main_thread = pthread_self();

intmax_t thread_id(pthread_t thread) {
    uint64_t tid = 0;
    pthread_threadid_np(thread, &tid);
    return static_cast<intmax_t>(tid);
}
}
#elif !defined(__ANDROID__)
#include <sys/syscall.h>
#endif

intmax_t xlogger_pid() {
    return getpid();
}

intmax_t xlogger_tid() {
#if defined(__APPLE__)
    return thread_id(nullptr);
#elif defined(__ANDROID__)
    return gettid();
#else
    return syscall(SYS_gettid);
#endif
}

intmax_t xlogger_maintid() {
#if defined(__APPLE__)
    static const intmax_t tid = thread_id(main_thread);
    return tid;
#else
    return xlogger_pid();
#endif
}
