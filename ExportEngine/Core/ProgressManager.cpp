#include "ProgressManager.h"

using namespace ATHC::EE;

ProgressManager &ProgressManager::instance()
{
    static ProgressManager mgr;
    return mgr;
}

void ProgressManager::startTask(const std::string &taskId,
                                int                totalSteps,
                                const std::string &message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    TaskProgress               &t = m_tasks[taskId];
    t.taskId                      = taskId;
    t.status                      = "running";
    t.current                     = 0;
    t.total                       = totalSteps;
    t.percent                     = 0.0;
    t.message                     = message;
}

void ProgressManager::updateTask(const std::string &taskId, int current, const std::string &message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto                        it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return;
    TaskProgress &t = it->second;
    t.current       = current;
    t.percent       = t.total > 0 ? (current * 100.0 / t.total) : 0.0;
    if (!message.empty())
        t.message = message;
}

void ProgressManager::finishTask(const std::string &taskId, const std::string &message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto                        it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return;
    TaskProgress &t = it->second;
    t.status        = "done";
    t.current       = t.total;
    t.percent       = 100.0;
    if (!message.empty())
        t.message = message;
}

void ProgressManager::failTask(const std::string &taskId, const std::string &message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto                        it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return;
    TaskProgress &t = it->second;
    t.status        = "error";
    if (!message.empty())
        t.message = message;
}

TaskProgress ProgressManager::getTask(const std::string &taskId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto                        it = m_tasks.find(taskId);
    if (it != m_tasks.end())
        return it->second;
    return TaskProgress{ taskId, "idle", 0, 0, 0.0, "task not found" };
}

std::unordered_map<std::string, TaskProgress> ProgressManager::getAllTasks() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tasks;
}
