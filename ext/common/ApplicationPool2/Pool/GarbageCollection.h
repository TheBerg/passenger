/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */

// This file is included inside the Pool class.

private:

struct GarbageCollectorState {
	unsigned long long now;
	unsigned long long nextGcRunTime;
	boost::container::vector<Callback> actions;
};

boost::condition_variable garbageCollectionCond;


static void garbageCollect(PoolPtr self) {
	TRACE_POINT();
	{
		ScopedLock lock(self->syncher);
		self->garbageCollectionCond.timed_wait(lock,
			posix_time::seconds(5));
	}
	while (!this_thread::interruption_requested()) {
		try {
			UPDATE_TRACE_POINT();
			unsigned long long sleepTime = self->realGarbageCollect();
			UPDATE_TRACE_POINT();
			ScopedLock lock(self->syncher);
			self->garbageCollectionCond.timed_wait(lock,
				posix_time::microseconds(sleepTime));
		} catch (const thread_interrupted &) {
			break;
		} catch (const tracable_exception &e) {
			P_WARN("ERROR: " << e.what() << "\n  Backtrace:\n" << e.backtrace());
		}
	}
}

void maybeUpdateNextGcRuntime(GarbageCollectorState &state, unsigned long candidate) {
	if (state.nextGcRunTime == 0 || candidate < state.nextGcRunTime) {
		state.nextGcRunTime = candidate;
	}
}

void checkWhetherProcessCanBeGarbageCollected(GarbageCollectorState &state,
	const GroupPtr &group, const ProcessPtr &process, ProcessList &output)
{
	assert(maxIdleTime > 0);
	unsigned long long processGcTime = process->lastUsed + maxIdleTime;
	if (process->sessions == 0
	 && state.now >= processGcTime
	 && (unsigned long) group->getProcessCount() > group->options.minProcesses)
	{
		if (output.capacity() == 0) {
			output.reserve(group->enabledCount);
		}
		output.push_back(process);
	} else {
		maybeUpdateNextGcRuntime(state, processGcTime);
	}
}

void garbageCollectProcessesInGroup(GarbageCollectorState &state,
	const GroupPtr &group)
{
	ProcessList &processes = group->enabledProcesses;
	ProcessList processesToGc;
	ProcessList::iterator p_it, p_end = processes.end();

	for (p_it = processes.begin(); p_it != p_end; p_it++) {
		const ProcessPtr &process = *p_it;
		checkWhetherProcessCanBeGarbageCollected(state, group, process,
			processesToGc);
	}

	p_end = processesToGc.end();
	for (p_it = processesToGc.begin(); p_it != p_end; p_it++) {
		ProcessPtr process = *p_it;
		P_DEBUG("Garbage collect idle process: " << process->inspect() <<
			", group=" << group->name);
		group->detach(process, state.actions);
	}
}

void maybeCleanPreloader(GarbageCollectorState &state, const GroupPtr &group) {
	if (group->spawner->cleanable() && group->options.getMaxPreloaderIdleTime() != 0) {
		unsigned long long spawnerGcTime =
			group->spawner->lastUsed() +
			group->options.getMaxPreloaderIdleTime() * 1000000;
		if (state.now >= spawnerGcTime) {
			P_DEBUG("Garbage collect idle spawner: group=" << group->name);
			group->cleanupSpawner(state.actions);
		} else {
			maybeUpdateNextGcRuntime(state, spawnerGcTime);
		}
	}
}

unsigned long long
realGarbageCollect() {
	TRACE_POINT();
	ScopedLock lock(syncher);
	SuperGroupMap::ConstIterator sg_it(superGroups);
	GarbageCollectorState state;
	state.now = SystemTime::getUsec();
	state.nextGcRunTime = 0;

	P_DEBUG("Garbage collection time...");
	verifyInvariants();

	// For all supergroups and groups...
	while (*sg_it != NULL) {
		const SuperGroupPtr superGroup = sg_it.getValue();
		SuperGroup::GroupList &groups = superGroup->groups;
		SuperGroup::GroupList::iterator g_it, g_end = groups.end();

		superGroup->verifyInvariants();

		for (g_it = groups.begin(); g_it != g_end; g_it++) {
			GroupPtr group = *g_it;

			if (maxIdleTime > 0) {
				// ...detach processes that have been idle for more than maxIdleTime.
				garbageCollectProcessesInGroup(state, group);
			}

			group->verifyInvariants();

			// ...cleanup the spawner if it's been idle for more than preloaderIdleTime.
			maybeCleanPreloader(state, group);
		}

		superGroup->verifyInvariants();
		sg_it.next();
	}

	verifyInvariants();
	lock.unlock();

	// Schedule next garbage collection run.
	unsigned long long sleepTime;
	if (state.nextGcRunTime == 0 || state.nextGcRunTime <= state.now) {
		if (maxIdleTime == 0) {
			sleepTime = 10 * 60 * 1000000;
		} else {
			sleepTime = maxIdleTime;
		}
	} else {
		sleepTime = state.nextGcRunTime - state.now;
	}
	P_DEBUG("Garbage collection done; next garbage collect in " <<
		std::fixed << std::setprecision(3) << (sleepTime / 1000000.0) << " sec");

	UPDATE_TRACE_POINT();
	runAllActions(state.actions);
	UPDATE_TRACE_POINT();
	state.actions.clear();
	return sleepTime;
}


protected:

void initializeGarbageCollection() {
	interruptableThreads.create_thread(
		boost::bind(garbageCollect, shared_from_this()),
		"Pool garbage collector",
		POOL_HELPER_THREAD_STACK_SIZE
	);
}

void wakeupGarbageCollector() {
	garbageCollectionCond.notify_all();
}