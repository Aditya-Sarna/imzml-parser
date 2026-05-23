// ---------------------------------------------------------------------------
// OpenMS/CONCEPT/ProgressLogger.h
// Console progress logger — mirrors OpenMS ProgressLogger interface
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <chrono>

namespace OpenMS
{

class ProgressLogger
{
public:
    enum LogType { CMD, GUI, NONE };

    ProgressLogger() = default;
    virtual ~ProgressLogger() = default;

    void setLogType(LogType t) { logType_ = t; }
    LogType getLogType() const { return logType_; }

    void startProgress(std::size_t begin, std::size_t end,
                       const std::string& label) const
    {
        if (logType_ == NONE) return;
        begin_  = begin;
        end_    = end;
        label_  = label;
        current_ = begin;
        start_  = std::chrono::steady_clock::now();
        if (logType_ == CMD)
            fprintf(stderr, "  %s", label.c_str());
    }

    void setProgress(std::size_t val) const
    {
        if (logType_ == NONE) return;
        current_ = val;
        if (logType_ == CMD)
        {
            std::size_t range = end_ > begin_ ? end_ - begin_ : 1;
            int pct = static_cast<int>(100.0 * (val - begin_) / range);
            fprintf(stderr, "\r  %s [%3d%%]", label_.c_str(), pct);
            fflush(stderr);
        }
    }

    void endProgress() const
    {
        if (logType_ == NONE) return;
        using namespace std::chrono;
        auto elapsed = duration_cast<milliseconds>(
                           steady_clock::now() - start_).count();
        if (logType_ == CMD)
            fprintf(stderr, "\r  %s [done] (%.2f s)\n",
                    label_.c_str(), elapsed / 1000.0);
    }

private:
    LogType       logType_  {NONE};
    mutable std::size_t begin_ {0}, end_ {0}, current_ {0};
    mutable std::string label_;
    mutable std::chrono::steady_clock::time_point start_;
};

} // namespace OpenMS
