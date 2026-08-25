#include "CommandQueue.hpp"

#include <exception>

namespace simple_rcon
{
    CommandQueue::CommandQueue(Executor executor) : m_executor(std::move(executor)) {}

    std::string CommandQueue::enqueue_and_wait(const std::string& command, std::chrono::milliseconds timeout)
    {
        auto task = std::make_shared<Task>();
        task->command = command;
        auto future = task->result.get_future();

        {
            std::scoped_lock lock{m_mutex};
            if (m_stopping)
            {
                return "error: command queue is shutting down";
            }
            m_tasks.push(task);
        }

        if (future.wait_for(timeout) == std::future_status::ready)
        {
            return future.get();
        }
        return "error: command timed out waiting for the game thread";
    }

    int CommandQueue::drain(int max_items)
    {
        int drained = 0;
        while (drained < max_items)
        {
            std::shared_ptr<Task> task;
            {
                std::scoped_lock lock{m_mutex};
                if (m_tasks.empty())
                {
                    break;
                }
                task = m_tasks.front();
                m_tasks.pop();
            }

            try
            {
                task->result.set_value(m_executor(task->command));
            }
            catch (const std::exception& e)
            {
                task->result.set_value(std::string{"error: exception while executing command: "} + e.what());
            }
            catch (...)
            {
                task->result.set_value("error: unknown exception while executing command");
            }
            ++drained;
        }
        return drained;
    }

    void CommandQueue::shutdown()
    {
        std::queue<std::shared_ptr<Task>> pending;
        {
            std::scoped_lock lock{m_mutex};
            m_stopping = true;
            std::swap(pending, m_tasks);
        }
        while (!pending.empty())
        {
            pending.front()->result.set_value("error: command queue stopped before execution");
            pending.pop();
        }
    }
}
