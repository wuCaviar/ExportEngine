#pragma once

#include <string>
#include <mutex>
#include <unordered_map>

namespace ATHC::EE {

struct TaskProgress
{
    std::string taskId;
    std::string status; // "idle" | "running" | "done" | "error"
    int         current = 0;
    int         total   = 0;
    double      percent = 0.0;
    std::string message;
};

class ProgressManager
{
public:
    static ProgressManager &instance();

    // Task lifecycle
    void startTask(const std::string &taskId, int totalSteps, const std::string &message = "");
    void updateTask(const std::string &taskId, int current, const std::string &message = "");
    void finishTask(const std::string &taskId, const std::string &message = "");
    void failTask(const std::string &taskId, const std::string &message = "");

    // Query
    TaskProgress                                  getTask(const std::string &taskId) const;
    std::unordered_map<std::string, TaskProgress> getAllTasks() const;

private:
    ProgressManager() = default;
    mutable std::mutex                            m_mutex;
    std::unordered_map<std::string, TaskProgress> m_tasks;
};

} // namespace ATHC::EE
