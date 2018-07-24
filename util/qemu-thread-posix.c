/*
 * Wrappers around mutex/cond/thread functions
 *
 * Copyright Red Hat, Inc. 2009
 *
 * Author:
 *  Marcelo Tosatti <mtosatti@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include "qemu/osdep.h"
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/futex.h>
#endif
#include "qemu/abort.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "qemu/notify.h"
#include "sysemu/sysemu.h"
#include "trace.h"

#include <stdbool.h>

/* |is_qemu_thread| is |true| if the current thread was launched with
 * qemu_thread_create().
 * The main thread is the only thread that's supposed to not be like that. */
static __thread bool is_qemu_thread = false;

static bool name_threads;
static QemuThreadSetupFunc thread_setup_func;

void qemu_thread_register_setup_callback(QemuThreadSetupFunc setup_func)
{
    thread_setup_func = setup_func;
}

void qemu_thread_naming(bool enable)
{
    name_threads = enable;

#ifndef CONFIG_THREAD_SETNAME_BYTHREAD
    /* This is a debugging option, not fatal */
    if (enable) {
        fprintf(stderr, "qemu: thread naming not supported on this host\n");
    }
#endif
}

static void error_exit(int err, const char *msg)
{
    qemu_abort("qemu: %s: %s\n", msg, strerror(err));
}

void qemu_mutex_init(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_init(&mutex->lock, NULL);
    if (err)
        error_exit(err, __func__);
    mutex->initialized = true;
}

void qemu_mutex_destroy(QemuMutex *mutex)
{
    int err;

    assert(mutex->initialized);
    mutex->initialized = false;
    err = pthread_mutex_destroy(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_lock_impl(QemuMutex *mutex, const char *file, const int line)
{
    int err;

    assert(mutex->initialized);
    trace_qemu_mutex_lock(mutex, file, line);

    err = pthread_mutex_lock(&mutex->lock);
    if (err)
        error_exit(err, __func__);

    trace_qemu_mutex_locked(mutex, file, line);
}

int qemu_mutex_trylock_impl(QemuMutex *mutex, const char *file, const int line)
{
    int err;

    assert(mutex->initialized);
    err = pthread_mutex_trylock(&mutex->lock);
    if (err == 0) {
        trace_qemu_mutex_locked(mutex, file, line);
        return 0;
    }
    if (err != EBUSY) {
        error_exit(err, __func__);
    }
    return -EBUSY;
}

void qemu_mutex_unlock_impl(QemuMutex *mutex, const char *file, const int line)
{
    int err;

    assert(mutex->initialized);
    err = pthread_mutex_unlock(&mutex->lock);
    if (err)
        error_exit(err, __func__);

    trace_qemu_mutex_unlock(mutex, file, line);
}

void qemu_rec_mutex_init(QemuRecMutex *mutex)
{
    int err;
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    err = pthread_mutex_init(&mutex->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (err) {
        error_exit(err, __func__);
    }
    mutex->initialized = true;
}

void qemu_cond_init(QemuCond *cond)
{
    int err;

    err = pthread_cond_init(&cond->cond, NULL);
    if (err)
        error_exit(err, __func__);
    cond->initialized = true;
}

void qemu_cond_destroy(QemuCond *cond)
{
    int err;

    assert(cond->initialized);
    cond->initialized = false;
    err = pthread_cond_destroy(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_signal(QemuCond *cond)
{
    int err;

    assert(cond->initialized);
    err = pthread_cond_signal(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_broadcast(QemuCond *cond)
{
    int err;

    assert(cond->initialized);
    err = pthread_cond_broadcast(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_wait_impl(QemuCond *cond, QemuMutex *mutex, const char *file, const int line)
{
    int err;

    assert(cond->initialized);
    trace_qemu_mutex_unlock(mutex, file, line);
    err = pthread_cond_wait(&cond->cond, &mutex->lock);
    trace_qemu_mutex_locked(mutex, file, line);
    if (err)
        error_exit(err, __func__);
}

void qemu_sem_init(QemuSemaphore *sem, int init)
{
    int rc;

#ifndef CONFIG_SEM_TIMEDWAIT
    rc = pthread_mutex_init(&sem->lock, NULL);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
    rc = pthread_cond_init(&sem->cond, NULL);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
    if (init < 0) {
        error_exit(EINVAL, __func__);
    }
    sem->count = init;
#else
    rc = sem_init(&sem->sem, 0, init);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
    sem->initialized = true;
}

void qemu_sem_destroy(QemuSemaphore *sem)
{
    int rc;

    assert(sem->initialized);
    sem->initialized = false;
#ifndef CONFIG_SEM_TIMEDWAIT
    rc = pthread_cond_destroy(&sem->cond);
    if (rc < 0) {
        error_exit(rc, __func__);
    }
    rc = pthread_mutex_destroy(&sem->lock);
    if (rc < 0) {
        error_exit(rc, __func__);
    }
#else
    rc = sem_destroy(&sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

void qemu_sem_post(QemuSemaphore *sem)
{
    int rc;

    assert(sem->initialized);
#ifndef CONFIG_SEM_TIMEDWAIT
    pthread_mutex_lock(&sem->lock);
    if (sem->count == UINT_MAX) {
        rc = EINVAL;
    } else {
        sem->count++;
        rc = pthread_cond_signal(&sem->cond);
    }
    pthread_mutex_unlock(&sem->lock);
    if (rc != 0) {
        error_exit(rc, __func__);
    }
#else
    rc = sem_post(&sem->sem);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

static void compute_abs_deadline(struct timespec *ts, int ms)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_nsec = tv.tv_usec * 1000 + (ms % 1000) * 1000000;
    ts->tv_sec = tv.tv_sec + ms / 1000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

int qemu_sem_timedwait(QemuSemaphore *sem, int ms)
{
    int rc;
    struct timespec ts;

    assert(sem->initialized);
#ifndef CONFIG_SEM_TIMEDWAIT
    rc = 0;
    compute_abs_deadline(&ts, ms);
    pthread_mutex_lock(&sem->lock);
    while (sem->count == 0) {
        rc = pthread_cond_timedwait(&sem->cond, &sem->lock, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
        if (rc != 0) {
            error_exit(rc, __func__);
        }
    }
    if (rc != ETIMEDOUT) {
        --sem->count;
    }
    pthread_mutex_unlock(&sem->lock);
    return (rc == ETIMEDOUT ? -1 : 0);
#else
    if (ms <= 0) {
        /* This is cheaper than sem_timedwait.  */
        do {
            rc = sem_trywait(&sem->sem);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1 && errno == EAGAIN) {
            return -1;
        }
    } else {
        compute_abs_deadline(&ts, ms);
        do {
            rc = sem_timedwait(&sem->sem, &ts);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1 && errno == ETIMEDOUT) {
            return -1;
        }
    }
    if (rc < 0) {
        error_exit(errno, __func__);
    }
    return 0;
#endif
}

void qemu_sem_wait(QemuSemaphore *sem)
{
    int rc;

    assert(sem->initialized);
#ifndef CONFIG_SEM_TIMEDWAIT
    pthread_mutex_lock(&sem->lock);
    while (sem->count == 0) {
        rc = pthread_cond_wait(&sem->cond, &sem->lock);
        if (rc != 0) {
            error_exit(rc, __func__);
        }
    }
    --sem->count;
    pthread_mutex_unlock(&sem->lock);
#else
    do {
        rc = sem_wait(&sem->sem);
    } while (rc == -1 && errno == EINTR);
    if (rc < 0) {
        error_exit(errno, __func__);
    }
#endif
}

#ifdef __linux__
#include "qemu/futex.h"
#else
static inline void qemu_futex_wake(QemuEvent *ev, int n)
{
    assert(ev->initialized);
    pthread_mutex_lock(&ev->lock);
    if (n == 1) {
        pthread_cond_signal(&ev->cond);
    } else {
        pthread_cond_broadcast(&ev->cond);
    }
    pthread_mutex_unlock(&ev->lock);
}

static inline void qemu_futex_wait(QemuEvent *ev, unsigned val)
{
    assert(ev->initialized);
    pthread_mutex_lock(&ev->lock);
    if (ev->value == val) {
        pthread_cond_wait(&ev->cond, &ev->lock);
    }
    pthread_mutex_unlock(&ev->lock);
}
#endif

/* Valid transitions:
 * - free->set, when setting the event
 * - busy->set, when setting the event, followed by qemu_futex_wake
 * - set->free, when resetting the event
 * - free->busy, when waiting
 *
 * set->busy does not happen (it can be observed from the outside but
 * it really is set->free->busy).
 *
 * busy->free provably cannot happen; to enforce it, the set->free transition
 * is done with an OR, which becomes a no-op if the event has concurrently
 * transitioned to free or busy.
 */

#define EV_SET         0
#define EV_FREE        1
#define EV_BUSY       -1

void qemu_event_init(QemuEvent *ev, bool init)
{
#ifndef __linux__
    pthread_mutex_init(&ev->lock, NULL);
    pthread_cond_init(&ev->cond, NULL);
#endif

    ev->value = (init ? EV_SET : EV_FREE);
    ev->initialized = true;
}

void qemu_event_destroy(QemuEvent *ev)
{
    assert(ev->initialized);
    ev->initialized = false;
#ifndef __linux__
    pthread_mutex_destroy(&ev->lock);
    pthread_cond_destroy(&ev->cond);
#endif
}

void qemu_event_set(QemuEvent *ev)
{
    /* qemu_event_set has release semantics, but because it *loads*
     * ev->value we need a full memory barrier here.
     */
    assert(ev->initialized);
    smp_mb();
    if (atomic_read(&ev->value) != EV_SET) {
        if (atomic_xchg(&ev->value, EV_SET) == EV_BUSY) {
            /* There were waiters, wake them up.  */
            qemu_futex_wake(ev, INT_MAX);
        }
    }
}

void qemu_event_reset(QemuEvent *ev)
{
    unsigned value;

    assert(ev->initialized);
    value = atomic_read(&ev->value);
    smp_mb_acquire();
    if (value == EV_SET) {
        /*
         * If there was a concurrent reset (or even reset+wait),
         * do nothing.  Otherwise change EV_SET->EV_FREE.
         */
        atomic_or(&ev->value, EV_FREE);
    }
}

void qemu_event_wait(QemuEvent *ev)
{
    unsigned value;

    assert(ev->initialized);
    value = atomic_read(&ev->value);
    smp_mb_acquire();
    if (value != EV_SET) {
        if (value == EV_FREE) {
            /*
             * Leave the event reset and tell qemu_event_set that there
             * are waiters.  No need to retry, because there cannot be
             * a concurrent busy->free transition.  After the CAS, the
             * event will be either set or busy.
             */
            if (atomic_cmpxchg(&ev->value, EV_FREE, EV_BUSY) == EV_SET) {
                return;
            }
        }
        qemu_futex_wait(ev, EV_BUSY);
    }
}

static pthread_key_t exit_key;

union NotifierThreadData {
    void *ptr;
    NotifierList list;
};
QEMU_BUILD_BUG_ON(sizeof(union NotifierThreadData) != sizeof(void *));

void qemu_thread_atexit_add(Notifier *notifier)
{
    if (is_qemu_thread) {
        union NotifierThreadData ntd;
        ntd.ptr = pthread_getspecific(exit_key);
        notifier_list_add(&ntd.list, notifier);
        pthread_setspecific(exit_key, ntd.ptr);
    } else {
        /* this is a main thread, use the main QEMU notifier list */
        qemu_add_exit_notifier(notifier);
    }
}

void qemu_thread_atexit_remove(Notifier *notifier)
{
    if (is_qemu_thread) {
        union NotifierThreadData ntd;
        ntd.ptr = pthread_getspecific(exit_key);
        notifier_remove(notifier);
        pthread_setspecific(exit_key, ntd.ptr);
    } else {
        /* this is a main thread, use the main QEMU notifier list */
        qemu_remove_exit_notifier(notifier);
    }
}

static void qemu_thread_atexit_run(void *arg)
{
    union NotifierThreadData ntd = { .ptr = arg };
    notifier_list_notify(&ntd.list, NULL);
}

static void __attribute__((constructor)) qemu_thread_atexit_init(void)
{
    pthread_key_create(&exit_key, qemu_thread_atexit_run);
}


/* Attempt to set the threads name; note that this is for debug, so
 * we're not going to fail if we can't set it.
 */
static void qemu_thread_set_name(QemuThread *thread, const char *name)
{
#ifdef CONFIG_PTHREAD_SETNAME_NP
    pthread_setname_np(thread->thread, name);
#endif
}

/* A small data structure to group the thread startup parameters.
 * This is passed to the trampoline function below which will first
 * setup AndroidEmu to use the QEMU-based looper implementation before
 * calling the thread's start routine. This ensures that any QEMU code
 * that calls into AndroidEmu functions work properly when it uses
 * timers and file i/o watches.
 *
 * Note that the ThreadStartData instance is created on the heap by
 * qemu_thread_create() and its pointer passed to the trampoline through
 * pthread_create(). This effectively transfers ownership of the object
 * to the trampoline which is responsible for freeing it.
 */
typedef struct {
    void *(*start_routine)(void*);
    void* arg;
} ThreadStartData;

static void* qemu_thread_trampoline(void* data_) {
    is_qemu_thread = true;

    /* Copy heap-allocated data to stack then release it. */
    ThreadStartData data = *(ThreadStartData*)data_;
    free(data_);

    if (thread_setup_func)
        (*thread_setup_func)();

    /* Start the thread. */
    void* res = (*data.start_routine)(data.arg);

    /* Run the atexit notifiers deterministically, to make sure all the thread-
     * local variables are still alive (it's very easy to refer to some
     * __thread or pthread-specific variable from an exit notifier, and we
     * don't want to crash because of that). */
    void* tls_notifiers = pthread_getspecific(exit_key);
    qemu_thread_atexit_run(tls_notifiers);

    /* Make sure that the following call from the TLS destructor does nothing,
     * even if some notifier tried to add another one. I hope nobody actually
     * does anything like that. */
    pthread_setspecific(exit_key, NULL);

    return res;
}

void qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void*),
                       void *arg, int mode)
{
    sigset_t set, oldset;
    int err;
    pthread_attr_t attr;

    err = pthread_attr_init(&attr);
    if (err) {
        error_exit(err, __func__);
    }

    /* Leave signal handling to the iothread.  */
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, &oldset);

    /* Create heap-allocated ThreadStartData object and pass its ownership
     * to the trampoline. */
    ThreadStartData* data = malloc(sizeof(*data));
    data->start_routine = start_routine;
    data->arg = arg;

    err = pthread_create(&thread->thread, &attr, qemu_thread_trampoline, data);
    if (err) {
        free(data);
        error_exit(err, __func__);
    }

    if (name_threads) {
        qemu_thread_set_name(thread, name);
    }

    if (mode == QEMU_THREAD_DETACHED) {
        err = pthread_detach(thread->thread);
        if (err) {
            error_exit(err, __func__);
        }
    }
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    pthread_attr_destroy(&attr);
}

void qemu_thread_get_self(QemuThread *thread)
{
    thread->thread = pthread_self();
}

bool qemu_thread_is_self(QemuThread *thread)
{
   return pthread_equal(pthread_self(), thread->thread);
}

void qemu_thread_exit(void *retval)
{
    pthread_exit(retval);
}

void *qemu_thread_join(QemuThread *thread)
{
    int err;
    void *ret;

    err = pthread_join(thread->thread, &ret);
    if (err) {
        error_exit(err, __func__);
    }
    return ret;
}
