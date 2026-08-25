#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace simple_rcon
{
    class CommandQueue
    {
      public:
        using Executor = std::function<std::string(const std::string&)>;

        explicit CommandQueue(Executor executor);

        std::string enqueue_and_wait(const std::string& command, std::chrono::milliseconds timeout);
        int drain(int max_items = 32);
        void shutdown();

      private:
        struct Task
        {
            std::string command;
            std::promise<std::string> result;
        };

        Executor m_executor;
        std::mutex m_mutex;
        std::queue<std::shared_ptr<Task>> m_tasks;
        bool m_stopping{false};
    };
}
