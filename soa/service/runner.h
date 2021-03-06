/* runner.h                                                        -*- C++ -*-
   Wolfgang Sourdeau, September 2013
   Copyright (c) 2013 Datacratic.  All rights reserved.

   A command runner class that hides the specifics of the underlying unix
   system calls and can intercept input and output.
*/

#pragma once

#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "soa/types/value_description.h"

#include "epoller.h"
#include "sink.h"

/* value descriptions for "timeval" and "rusage" */

CREATE_STRUCTURE_DESCRIPTION(timeval);
CREATE_STRUCTURE_DESCRIPTION(rusage);


namespace Datacratic {

/*****************************************************************************/
/* RUN RESULT                                                                */
/*****************************************************************************/

/** This is the result that is returned that encapsulates the state of a
    command that ran.

    There are 3 broad outcomes possible:
    1.  There was an error launching;
    2.  The command exited due to a signal;
    3.  The command exited normally and gave us a return code.

    Note that recording good messages for launch errors is really important,
    as it can be very difficult to debug this kind of error.
*/

struct RunResult {
    RunResult();

    /** Update the state in response to the command returning.
        The status parameter is as returned by waidpid.
    */
    void updateFromStatus(int status);

    /** Extract the process return code as would be returned by a shell. */
    int processStatus() const;

    /** Update the state in response to a launch error. */
    void updateFromLaunchError(int launchErrno,
                               const std::string & launchError);

    /// Enumeration of the final state of the command
    enum State {
        UNKNOWN,        ///< State is not known
        LAUNCH_ERROR,   ///< Command was unable to be launched
        RETURNED,       ///< Command returned
        SIGNALED        ///< Command exited with a signal
    };
        
    State state;
    int signum;         ///< Signal number it returned with
    int returnCode;     ///< Return code if command exited

    int launchErrno;    ///< Errno (if appropriate) of launch error
    std::string launchError;  ///< Error string describing launch error

    rusage usage;       ///< Process statistics
};

std::string to_string(const RunResult::State & state);

std::ostream &
operator << (std::ostream & stream, const RunResult::State & state);

CREATE_STRUCTURE_DESCRIPTION(RunResult);
CREATE_ENUM_DESCRIPTION_NAMED(RunResultStateDescription, RunResult::State);


/*****************************************************************************/
/* RUNNER                                                                    */
/*****************************************************************************/

/** This class encapsulates running a sub-command, including launching it and
    controlling the input, output and error streams of the subprocess.
*/

struct Runner: public Epoller {
    typedef std::function<void (const RunResult & result)> OnTerminate;

    Runner();
    ~Runner();

    OutputSink & getStdInSink();

    /** Run the subprocess. */
    void run(const std::vector<std::string> & command,
             const OnTerminate & onTerminate = nullptr,
             const std::shared_ptr<InputSink> & stdOutSink = nullptr,
             const std::shared_ptr<InputSink> & stdErrSink = nullptr);

    /** Kill the subprocess with the given signal, then wait for it to
        terminate. */
    void kill(int signal = SIGTERM) const;

    /** Send the given signal, but don't wait for it to terminate. */
    void signal(int signum);

    /** Synchronous wait for the subprocess to start.  Returns true if the
        process started, or false if it wasn't able to start.

        Will wait for a maximum of secondsToWait seconds.
    */
    bool waitStart(double secondsToWait = INFINITY) const;

    /** Synchronous wait for termination of the subprocess. */
    void waitTermination() const;

    /** Is the subprocess running? */
    bool running() const { return running_; }

    /** Process ID of the child process.  Returns -1 if not running. */
    pid_t childPid() const { return childPid_; }

private:
    struct Task {
        struct ChildFds {
            ChildFds();

            void closeRemainingFds();
            void dupToStdStreams();
            void close();

            int stdIn;
            int stdOut;
            int stdErr;
            int statusFd;
        };

        /** State of the process. */
        enum StatusState {
            ST_UNKNOWN,    ///< Unknown status
            LAUNCHING,     ///< Being launched
            RUNNING,       ///< Currently running
            STOPPED,       ///< No longer running
            DONE           ///< Completely stopped
        };

        /** Possible errors that could happen in launching.  These are
            enumerated here so that they can be passed back as an int
            rather than as a variable length string (or a const char *
            to memory which we could have to ensure was available in
            both the launcher process and the calling process).
        */
        enum LaunchErrorCode {
            E_NONE,                     ///< No launch error
            E_READ_STATUS_PIPE,         ///< Error reading status pipe
            E_STATUS_PIPE_WRONG_LENGTH, ///< Status msg wrong length
            E_SUBTASK_LAUNCH,           ///< Error launching subtask
            E_SUBTASK_WAITPID,          ///< Error calling waitpid
            E_WRONG_CHILD               ///< Wrong child was reaped
        };

        /** Turn a launch error code into a descriptive string. */
        static std::string strLaunchError(LaunchErrorCode error);
            
        /** Structure passed back and forth between the launcher and the
            monitor to know the current state of the running process.
        */
        struct ChildStatus {
            ChildStatus();

            StatusState state;
            pid_t pid;
            int childStatus;
            int launchErrno;
            LaunchErrorCode launchErrorCode;
            rusage usage;
        };

        Task();

        void setupInSink();
        void flushInSink();
        void flushStdInBuffer();
        void runWrapper(const std::vector<std::string> & command,
                        ChildFds & fds);
                        
        void postTerminate(Runner & runner);

        std::vector<std::string> command;
        OnTerminate onTerminate;
        RunResult runResult;

        pid_t wrapperPid;

        int stdInFd;
        int stdOutFd;
        int stdErrFd;
        int statusFd;

        StatusState statusState;
        static std::string statusStateAsString(StatusState statusState);
    };

    void prepareChild();
    bool handleEpollEvent(const struct epoll_event & event);
    void handleChildStatus(const struct epoll_event & event);
    void handleOutputStatus(const struct epoll_event & event,
                            int fd, std::shared_ptr<InputSink> & sink);
    void handleWakeup(const struct epoll_event & event);

    void attemptTaskTermination();

    int running_;

    /** Holds the child PID if > 0.  If not:
        -1 means the child has not launched yet
        -2 means there was a launch error
        -3 means the child has exited
    */
    pid_t childPid_;

    ML::Wakeup_Fd wakeup_;

    std::shared_ptr<AsyncFdOutputSink> stdInSink_;
    std::shared_ptr<InputSink> stdOutSink_;
    std::shared_ptr<InputSink> stdErrSink_;

    Task task_;
    char statusBuffer_[sizeof(Task::ChildStatus)];
    size_t statusRemaining_;
};


/*****************************************************************************/
/* EXECUTE                                                                   */
/*****************************************************************************/

/** These are free functions that take care of the details of setting up a
    Runner object and using it to run a single command.
*/

/** Execute a command synchronously using the specified message loop. */
RunResult execute(MessageLoop & loop,
                  const std::vector<std::string> & command,
                  const std::shared_ptr<InputSink> & stdOutSink = nullptr,
                  const std::shared_ptr<InputSink> & stdErrSink = nullptr,
                  const std::string & stdInData = "");

/** Execute a command synchronously using its own message loop. */
RunResult execute(const std::vector<std::string> & command,
                  const std::shared_ptr<InputSink> & stdOutSink = nullptr,
                  const std::shared_ptr<InputSink> & stdErrSink = nullptr,
                  const std::string & stdInData = "");

} // namespace Datacratic
