/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "pch.hpp"
#include <thread>

#include "lib/di/container.hpp"
#include "lib/thread/thread_pool.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "game/scheduling/task.hpp"

Dispatcher &Dispatcher::getInstance() {
	return inject<Dispatcher>();
}

void Dispatcher::init() {
	tasks.thread = std::jthread([&](std::stop_token stoken) {
		std::unique_lock lock(mutex);
		tasks.signal.wait(lock, [&]() -> bool {
			tasks.busy = true;

			if (!tasks.waitingList.empty()) {
				tasks.list.insert(tasks.list.end(), make_move_iterator(tasks.waitingList.begin()), make_move_iterator(tasks.waitingList.end()));
				tasks.waitingList.clear();
			}

			for (const auto &task : tasks.list) {
				if (task->hasTraceableContext()) {
					g_logger().trace("Executing task {}.", task->getContext());
				} else {
					g_logger().debug("Executing task {}.", task->getContext());
				}

				++dispatcherCycle;

				task->execute();
			}

			tasks.list.clear();

			try_notify();

			tasks.busy = false;

			return stoken.stop_requested();
		});
	});

	scheduledtasks.thread = std::jthread([&](std::stop_token stoken) {
		std::unique_lock lock(scheduledtasks.mutex);

		scheduledtasks.notify(100);

		scheduledtasks.signal.wait_until(lock, scheduledtasks.cycle, [&]() -> bool {
			const auto currentTime = std::chrono::system_clock::now();
			for (uint_fast64_t i = 0, max = scheduledtasks.list.size(); i < max && !scheduledtasks.list.empty(); ++i) {
				const auto &task = scheduledtasks.list.top();
				if (task->getTime() > currentTime) {
					scheduledtasks.cycle = task->getTime() + std::chrono::milliseconds(100);
					break;
				}

				addTask(task);

				if (!task->isCanceled() && task->isCycle()) {
					scheduledtasks.list.emplace(task);
				} else {
					scheduledtasks.map.erase(task->getEventId());
				}

				scheduledtasks.list.pop();
			}

			return stoken.stop_requested();
		});
		g_logger().info("hehe");
	});
}

void Dispatcher::addEvent(std::function<void(void)> f, const std::string &context) {
	addTask(std::make_shared<Task>(std::move(f), context));
}

void Dispatcher::addTask(const std::shared_ptr<Task> &task) {
	if (tasks.busy) {
		tasks.waitingList.emplace_back(task);
		return;
	}

	std::scoped_lock l(tasks.mutex);

	const bool doSignal = tasks.list.empty();
	tasks.list.emplace_back(task);

	if (doSignal) {
		tasks.signal.notify_all();
	}
}

uint64_t Dispatcher::scheduleEvent(const std::shared_ptr<Task> task) {
	std::scoped_lock l(scheduledtasks.mutex);

	task->setEventId(++lastEventId);

	scheduledtasks.list.emplace(task);
	scheduledtasks.map.emplace(task->getEventId(), task);

	scheduledtasks.notify(scheduledtasks.list.top()->getDelay());
	scheduledtasks.signal.notify_one();

	return task->getEventId();
}

uint64_t Dispatcher::scheduleEvent(uint32_t delay, std::function<void(void)> f, std::string context, bool cycle) {
	const auto &task = std::make_shared<Task>(std::move(f), std::move(context), delay, cycle);
	return scheduleEvent(task);
}

void Dispatcher::stopEvent(uint64_t eventId) {
	std::scoped_lock l(scheduledtasks.mutex);

	auto it = scheduledtasks.map.find(eventId);
	if (it == scheduledtasks.map.end()) {
		return;
	}

	it->second->cancel();
	scheduledtasks.map.erase(it);
}
