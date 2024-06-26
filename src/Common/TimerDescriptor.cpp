#if defined(OS_LINUX)
#include <Common/TimerDescriptor.h>
#include <Common/Exception.h>

#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_CREATE_TIMER;
    extern const int CANNOT_SET_TIMER_PERIOD;
    extern const int CANNOT_FCNTL;
    extern const int CANNOT_READ_FROM_SOCKET;
}

TimerDescriptor::TimerDescriptor(int clockid, int flags)
{
    timer_fd = timerfd_create(clockid, flags);
    if (timer_fd == -1)
        throw Exception(ErrorCodes::CANNOT_CREATE_TIMER, "Cannot create timer_fd descriptor");

    if (-1 == fcntl(timer_fd, F_SETFL, O_NONBLOCK))
        throw ErrnoException(ErrorCodes::CANNOT_FCNTL, "Cannot set O_NONBLOCK for timer_fd");
}

TimerDescriptor::TimerDescriptor(TimerDescriptor && other) noexcept : timer_fd(other.timer_fd)
{
    other.timer_fd = -1;
}

TimerDescriptor & TimerDescriptor::operator=(DB::TimerDescriptor && other) noexcept
{
    std::swap(timer_fd, other.timer_fd);
    return *this;
}

TimerDescriptor::~TimerDescriptor()
{
    /// Do not check for result cause cannot throw exception.
    if (timer_fd != -1)
    {
        int err = close(timer_fd);
        chassert(!err || errno == EINTR);
    }
}

void TimerDescriptor::reset() const
{
    itimerspec spec;
    spec.it_interval.tv_nsec = 0;
    spec.it_interval.tv_sec = 0;
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = 0;

    if (-1 == timerfd_settime(timer_fd, 0 /*relative timer */, &spec, nullptr))
        throw ErrnoException(ErrorCodes::CANNOT_SET_TIMER_PERIOD, "Cannot reset timer_fd");

    /// Drain socket.
    /// It may be possible that alarm happened and socket is readable.
    drain();
}

void TimerDescriptor::drain() const
{
    /// It is expected that socket returns 8 bytes when readable.
    /// Read in loop anyway cause signal may interrupt read call.
    uint64_t buf;
    while (true)
    {
        ssize_t res = ::read(timer_fd, &buf, sizeof(buf));
        if (res < 0)
        {
            if (errno == EAGAIN)
                break;

            if (errno != EINTR)
                throw ErrnoException(ErrorCodes::CANNOT_READ_FROM_SOCKET, "Cannot drain timer_fd");
        }
    }
}

void TimerDescriptor::setRelative(uint64_t usec) const
{
    static constexpr uint32_t TIMER_PRECISION = 1e6;

    itimerspec spec;
    spec.it_interval.tv_nsec = 0;
    spec.it_interval.tv_sec = 0;
    spec.it_value.tv_sec = usec / TIMER_PRECISION;
    spec.it_value.tv_nsec = (usec % TIMER_PRECISION) * 1'000;

    if (-1 == timerfd_settime(timer_fd, 0 /*relative timer */, &spec, nullptr))
        throw ErrnoException(ErrorCodes::CANNOT_SET_TIMER_PERIOD, "Cannot set time for timer_fd");
}

void TimerDescriptor::setRelative(Poco::Timespan timespan) const
{
    setRelative(timespan.totalMicroseconds());
}

}
#endif
