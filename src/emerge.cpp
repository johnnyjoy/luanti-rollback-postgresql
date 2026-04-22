// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2010-2013 kwolekr, Ryan Kwolek <kwolekr@minetest.net>


#include "emerge_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include "config.h"
#include "constants.h"
#include "irrlicht_changes/printing.h"
#include "filesys.h"
#include "log.h"
#include "serverenvironment.h"
#include "servermap.h"
#include "mapblock.h"
#include "mapgen/mg_biome.h"
#include "mapgen/mg_ore.h"
#include "mapgen/mg_decoration.h"
#include "mapgen/mg_schematic.h"
#include "porting.h"
#include "profiler.h"
#include "scripting_server.h"
#include "scripting_emerge.h"
#include "script/common/c_types.h" // LuaError
#include "server.h"
#include "settings.h"
#include "voxel.h"

namespace {
// Limit server-thread apply/prep work per step (fairness vs map send / peers).
constexpr u32 SERVER_EMERGE_APPLY_BATCH = 32;
// Max wait in tryWaitTakePreparedFirstPass if prep never delivers this pos.
constexpr auto PREP_WAIT_TIME_LIMIT = std::chrono::minutes(2);
} // namespace

EmergeParams::~EmergeParams()
{
	// Delete everything that was cloned on creation of EmergeParams
	delete biomegen;
	delete biomemgr;
	delete oremgr;
	delete decomgr;
	delete schemmgr;
}

EmergeParams::EmergeParams(EmergeManager *parent, const BiomeGen *biomegen,
	const BiomeManager *biomemgr,
	const OreManager *oremgr, const DecorationManager *decomgr,
	const SchematicManager *schemmgr) :
	ndef(parent->ndef),
	enable_mapgen_debug_info(parent->enable_mapgen_debug_info),
	gen_notify_on(parent->gen_notify_on),
	gen_notify_on_deco_ids(&parent->gen_notify_on_deco_ids),
	gen_notify_on_custom(&parent->gen_notify_on_custom),
	biomemgr(biomemgr->clone()), oremgr(oremgr->clone()),
	decomgr(decomgr->clone()), schemmgr(schemmgr->clone())
{
	this->biomegen = biomegen->clone(this->biomemgr);
}

////
//// EmergeManager
////

EmergeManager::EmergeManager(Server *server, MetricsBackend *mb)
{
	assert(server);
	this->m_server  = server;
	this->ndef      = server->ndef();
	this->biomemgr  = new BiomeManager(server);
	this->oremgr    = new OreManager(server);
	this->decomgr   = new DecorationManager(server);
	this->schemmgr  = new SchematicManager(server);

	// initialized later
	this->mgparams = nullptr;
	this->biomegen = nullptr;

	// Note that accesses to this variable are not synchronized.
	// This is because the *only* thread ever starting or stopping
	// EmergeThreads should be the ServerThread.

	enable_mapgen_debug_info = g_settings->getBool("enable_mapgen_debug_info");

	static_assert(ARRLEN(emergeActionStrs) == ARRLEN(m_completed_emerge_counter),
		"enum size mismatches");
	for (u32 i = 0; i < ARRLEN(m_completed_emerge_counter); i++) {
		std::string help_str("Number of completed emerges with status ");
		help_str.append(emergeActionStrs[i]);
		m_completed_emerge_counter[i] = mb->addCounter(
			"minetest_emerge_completed", help_str,
			{{"status", emergeActionStrs[i]}}
		);
	}

	m_qlimit_total = g_settings->getU32("emergequeue_limit_total");
	m_qlimit_diskonly = g_settings->getU32("emergequeue_limit_diskonly");
	m_qlimit_generate = g_settings->getU32("emergequeue_limit_generate");

	// don't trust user input for something very important like this
	m_qlimit_diskonly = rangelim(m_qlimit_diskonly, 2, 1000000);
	m_qlimit_generate = rangelim(m_qlimit_generate, 1, 1000000);
	m_qlimit_total = std::max(m_qlimit_total, std::max(m_qlimit_diskonly, m_qlimit_generate));
	// Bounded backpressure for server-thread apply of generated chunks; prep uses
	// the same order of magnitude for m_prepared_first (heap BlockMakeData).
	m_apply_queue_limit = rangelim(m_qlimit_total / 2, 4u, 256u);
	m_prep_inflight_limit = m_apply_queue_limit;
}

static std::unique_ptr<BlockMakeData> transferBlockMakeDataToHeap(BlockMakeData *stack)
{
	auto r = std::make_unique<BlockMakeData>();
	r->vmanip = stack->vmanip;
	stack->vmanip = nullptr;
	r->seed = stack->seed;
	r->blockpos_min = stack->blockpos_min;
	r->blockpos_max = stack->blockpos_max;
	r->nodedef = stack->nodedef;
	while (stack->transforming_liquid.size() > 0) {
		v3s16 v = stack->transforming_liquid.front();
		stack->transforming_liquid.pop_front();
		r->transforming_liquid.push_back(v);
	}
	return r;
}

static void transferBlockMakeDataFromUniqueToStack(
		std::unique_ptr<BlockMakeData> src, BlockMakeData *dst)
{
	if (!src || !dst)
		return;
	assert(!dst->vmanip);
	dst->vmanip = src->vmanip;
	src->vmanip = nullptr;
	dst->seed = src->seed;
	dst->blockpos_min = src->blockpos_min;
	dst->blockpos_max = src->blockpos_max;
	dst->nodedef = src->nodedef;
	while (src->transforming_liquid.size() > 0) {
		v3s16 v = src->transforming_liquid.front();
		src->transforming_liquid.pop_front();
		dst->transforming_liquid.push_back(v);
	}
}

namespace {

/**
 * First pass of emerge block acquisition (same logic as legacy getBlockOrStartGen body).
 * Call only while map/env access rules for @a map match the former call sites
 * (EmergeThread holds EnvAutoLock; server prep holds EnvAutoLock).
 */
EmergeAction emerge_map_get_block_or_start_gen(ServerMap *map, const v3s16 pos,
		bool allow_gen, const std::string *from_db, MapBlock **block,
		BlockMakeData *bmdata)
{
	auto block_ok = [] (MapBlock *b) {
		return b && b->isGenerated();
	};

	// 1). Attempt to fetch block from memory
	*block = map->getBlockNoCreateNoEx(pos);
	if (*block) {
		if (block_ok(*block)) {
			// If we just read it from the db but the block exists that means
			// someone else was faster. Don't touch it to prevent data loss.
			if (from_db)
				verbosestream << "getBlockOrStartGen: block loading raced" << std::endl;
			return EMERGE_FROM_MEMORY;
		}
	} else {
		if (!from_db) {
			// 2). We should attempt loading it
			return EMERGE_FROM_DISK;
		}
		// 2). Second invocation, we have the data
		if (!from_db->empty()) {
			*block = map->loadBlock(*from_db, pos);
			if (block_ok(*block))
				return EMERGE_FROM_DISK;
		}
	}

	// 3). Attempt to start generation
	if (allow_gen && map->initBlockMake(pos, bmdata))
		return EMERGE_GENERATED;

	// All attempts failed; cancel this block emerge
	return EMERGE_CANCELLED;
}
} // namespace


EmergeManager::~EmergeManager()
{
	for (u32 i = 0; i != m_threads.size(); i++) {
		EmergeThread *thread = m_threads[i];

		if (m_threads_active) {
			thread->stop();
			thread->signal();
			thread->wait();
		}

		delete thread;

		// Mapgen init might not be finished if there is an error during startup.
		if (m_mapgens.size() > i)
			delete m_mapgens[i];
	}

	delete biomegen;
	delete biomemgr;
	delete oremgr;
	delete decomgr;
	delete schemmgr;
}


BiomeManager *EmergeManager::getWritableBiomeManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return biomemgr;
}

OreManager *EmergeManager::getWritableOreManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return oremgr;
}

DecorationManager *EmergeManager::getWritableDecorationManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return decomgr;
}

SchematicManager *EmergeManager::getWritableSchematicManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return schemmgr;
}

void EmergeManager::initMap(MapDatabaseAccessor *holder)
{
	FATAL_ERROR_IF(m_db, "Map database already initialized.");
	assert(holder->dbase);
	m_db = holder;
}

void EmergeManager::resetMap()
{
	FATAL_ERROR_IF(m_threads_active, "Threads are still active.");
	m_db = nullptr;
}

void EmergeManager::initMapgens(MapgenParams *params)
{
	FATAL_ERROR_IF(!m_mapgens.empty(), "Mapgen already initialized.");

	mgparams = params;

	infostream << "EmergeManager: initializing for mapgen="
		<< Mapgen::getMapgenName(params->mgtype)
		<< " and chunksize=" << params->chunksize << std::endl;

	/*
	 * Singlenode is currently the only mapgen not affected by the
	 * unfinished slice bug, so allow multiple threads by default.
	 * We do this for the Lua mapgens who benefit from this (since singlenode
	 * itself isn't very useful).
	 * see <https://github.com/luanti-org/luanti/issues/9357>
	 */
	bool multithread = params->mgtype == MAPGEN_SINGLENODE;
	initThreads(multithread);

	v3s16 csize = params->chunksize * MAP_BLOCKSIZE;
	biomegen = biomemgr->createBiomeGen(BIOMEGEN_ORIGINAL, params->bparams, csize);

	for (u32 i = 0; i != m_threads.size(); i++) {
		EmergeParams *p = new EmergeParams(this, biomegen,
			biomemgr, oremgr, decomgr, schemmgr);
		m_mapgens.push_back(Mapgen::createMapgen(params->mgtype, params, p));
	}
}

void EmergeManager::initThreads(bool should_multithread)
{
	s16 nthreads = g_settings->getS16("num_emerge_threads");
	if (nthreads <= 0 && should_multithread) {
		u32 concurrency = Thread::getNumberOfProcessors();
		u32 memoryMB = porting::getMemorySizeMB();
		if (memoryMB) {
			// Cap threads according to total RAM with a conservative 1 GB per thread.
			// This is for the sake of Android phones, where many cores & low RAM
			// is not uncommon (e.g. 8C + 3GB).
			concurrency = std::min<u32>(concurrency, std::roundf(memoryMB / 1024.0f));
		}
		// Leave 2 cores for main thread and whatever else.
		nthreads = (concurrency > 2) ? (concurrency - 2) : 1;
		// Testing has shown that more than 4 threads don't become any faster:
		// <https://github.com/luanti-org/luanti/pull/16634>
		// May have to be revisited after emerge code is refactored to be less
		// lock heavy.
		nthreads = std::min<s16>(4, nthreads);
	}
	nthreads = std::max<s16>(1, nthreads);

	FATAL_ERROR_IF(!m_threads.empty(), "Threads already initialized.");
	for (s16 i = 0; i < nthreads; i++)
		m_threads.push_back(new EmergeThread(m_server, i));

	// Cap for m_prepared_first: must exceed worst skew between prep inbox order
	// (FIFO) and which mapblock each emerge thread waits on next (striped queues).
	m_prep_inflight_limit = std::max({
			m_apply_queue_limit,
			(u32)m_threads.size() * 512u,
			2048u});

	infostream << "EmergeManager: using " << nthreads << " thread(s)" << std::endl;
}

Mapgen *EmergeManager::getCurrentMapgen()
{
	if (!m_threads_active)
		return nullptr;

	for (u32 i = 0; i != m_threads.size(); i++) {
		EmergeThread *t = m_threads[i];
		if (t->isRunning() && t->isCurrentThread())
			return t->m_mapgen;
	}

	return nullptr;
}


void EmergeManager::startThreads()
{
	if (m_threads_active)
		return;

	m_apply_stopping = false;
	m_prep_stopping = false;

	for (u32 i = 0; i != m_threads.size(); i++)
		m_threads[i]->start();

	m_threads_active = true;
}


void EmergeManager::stopThreads()
{
	if (!m_threads_active)
		return;

	m_apply_stopping = true;
	m_apply_space_cv.notify_all();
	m_prep_stopping = true;
	m_prep_cv.notify_all();
	{
		std::lock_guard<std::mutex> lk(m_prep_mutex);
		m_prepared_first.clear();
	}
	{
		MutexAutoLock qlock(m_queue_mutex);
		while (!m_server_prep_inbox.empty())
			m_server_prep_inbox.pop();
	}
	if (m_server)
		processEmergeAppliesOnServerThread(m_server);

	// Request thread stop in parallel
	for (u32 i = 0; i != m_threads.size(); i++) {
		m_threads[i]->stop();
		m_threads[i]->signal();
	}

	// Then do the waiting for each
	for (u32 i = 0; i != m_threads.size(); i++)
		m_threads[i]->wait();

	m_threads_active = false;
}


bool EmergeManager::enqueueBlockEmerge(
	session_t peer_id,
	v3s16 blockpos,
	bool allow_generate,
	bool ignore_queue_limits)
{
	u16 flags = 0;
	if (allow_generate)
		flags |= BLOCK_EMERGE_ALLOW_GEN;
	if (ignore_queue_limits)
		flags |= BLOCK_EMERGE_FORCE_QUEUE;

	return enqueueBlockEmergeEx(blockpos, peer_id, flags, NULL, NULL);
}


bool EmergeManager::enqueueBlockEmergeEx(
	v3s16 blockpos,
	session_t peer_id,
	u16 flags,
	EmergeCompletionCallback callback,
	void *callback_param)
{
	EmergeThread *thread = NULL;
	bool entry_already_exists = false;

	{
		MutexAutoLock queuelock(m_queue_mutex);

		if (!pushBlockEmergeData(blockpos, peer_id, flags,
				callback, callback_param, &entry_already_exists))
			return false;

		if (entry_already_exists)
			return true;

		m_server_prep_inbox.push(blockpos);
		thread = getOptimalThread();
		thread->pushBlock(blockpos);
	}

	thread->signal();

	return true;
}


size_t EmergeManager::getQueueSize()
{
	MutexAutoLock queuelock(m_queue_mutex);
	return m_blocks_enqueued.size();
}

bool EmergeManager::isBlockInQueue(v3s16 pos)
{
	MutexAutoLock queuelock(m_queue_mutex);
	return m_blocks_enqueued.find(pos) != m_blocks_enqueued.end();
}


//
// Mapgen-related helper functions
//


v3s16 EmergeManager::getContainingChunk(v3s16 blockpos, v3s16 chunksize)
{
	v3s16 chunk_offset = -chunksize / 2;

	return getContainerPos(blockpos - chunk_offset, chunksize)
		* chunksize + chunk_offset;
}


int EmergeManager::getSpawnLevelAtPoint(v2s16 p)
{
	if (m_mapgens.empty() || !m_mapgens[0]) {
		errorstream << "EmergeManager: getSpawnLevelAtPoint() called"
			" before mapgen init" << std::endl;
		return 0;
	}

	return m_mapgens[0]->getSpawnLevelAtPoint(p);
}


// TODO(hmmmm): Move this to ServerMap
bool EmergeManager::isBlockUnderground(v3s16 blockpos)
{
	// Use a simple heuristic
	return blockpos.Y * (MAP_BLOCKSIZE + 1) <= mgparams->water_level;
}

bool EmergeManager::pushBlockEmergeData(
	v3s16 pos,
	u16 peer_requested,
	u16 flags,
	EmergeCompletionCallback callback,
	void *callback_param,
	bool *entry_already_exists)
{
	u32 &count_peer = m_peer_queue_count[peer_requested];

	if ((flags & BLOCK_EMERGE_FORCE_QUEUE) == 0) {
		if (m_blocks_enqueued.size() >= m_qlimit_total)
			return false;

		if (peer_requested != PEER_ID_INEXISTENT) {
			u32 qlimit_peer = (flags & BLOCK_EMERGE_ALLOW_GEN) ?
				m_qlimit_generate : m_qlimit_diskonly;
			if (count_peer >= qlimit_peer)
				return false;
		} else {
			// limit block enqueue requests for active blocks to 1/2 of total
			if (count_peer * 2 >= m_qlimit_total)
				return false;
		}
	}

	auto findres = m_blocks_enqueued.emplace(pos, BlockEmergeData());

	BlockEmergeData &bedata = findres.first->second;
	*entry_already_exists   = !findres.second;

	if (callback)
		bedata.callbacks.emplace_back(callback, callback_param);

	if (*entry_already_exists) {
		bedata.flags |= flags;
	} else {
		bedata.flags = flags;
		bedata.peer_requested = peer_requested;

		count_peer++;
	}

	return true;
}


bool EmergeManager::popBlockEmergeData(v3s16 pos, BlockEmergeData *bedata)
{
	auto it = m_blocks_enqueued.find(pos);
	if (it == m_blocks_enqueued.end())
		return false;

	*bedata = it->second;

	auto it2 = m_peer_queue_count.find(bedata->peer_requested);
	if (it2 == m_peer_queue_count.end())
		return false;

	u32 &count_peer = it2->second;

	assert(count_peer != 0);
	count_peer--;

	m_blocks_enqueued.erase(it);

	return true;
}


EmergeThread *EmergeManager::getOptimalThread()
{
	size_t nthreads = m_threads.size();

	FATAL_ERROR_IF(nthreads == 0, "No emerge threads!");

	size_t index = 0;
	size_t nitems_lowest = m_threads[0]->m_block_queue.size();

	for (size_t i = 1; i < nthreads; i++) {
		size_t nitems = m_threads[i]->m_block_queue.size();
		if (nitems < nitems_lowest) {
			index = i;
			nitems_lowest = nitems;
		}
	}

	return m_threads[index];
}

void EmergeManager::reportCompletedEmerge(EmergeAction action)
{
	assert((size_t)action < ARRLEN(m_completed_emerge_counter));
	m_completed_emerge_counter[(int)action]->increment();
}

void EmergeManager::runServerApplyItem(Server *server, EmergeApplyItem &&item)
{
	// Kind selects generated apply vs FROM_DISK second pass; one bounded queue
	// so shutdown/drain/backpressure stay a single story (see m_server_emerge_work_queue).
	// Splitting into two queues would mostly duplicate stopThreads() edge cases.
	assert(item.thread);
	EmergeThread *eth = item.thread;
	eth->m_apply_result_action = EMERGE_CANCELLED;
	eth->m_apply_result_block = nullptr;
	eth->m_apply_result_bm.reset();

	if (item.kind == EmergeApplyItem::FROM_DISK_SECOND_PASS) {
		ServerMap &smap = server->m_env->getServerMap();
		MapBlock *block = nullptr;
		BlockMakeData bmdata;
		EmergeAction action;
		{
			Server::EnvAutoLock envlock(server);
			ScopeProfiler sp_disk(g_profiler,
				"Server: emerge disk: getBlockOrStartGen", SPT_AVG, PRECISION_MICRO);
			action = emerge_map_get_block_or_start_gen(
					&smap, item.pos, item.allow_gen, &item.from_db, &block, &bmdata);
		}
		assert(action == EMERGE_FROM_MEMORY || action == EMERGE_FROM_DISK ||
				action == EMERGE_GENERATED || action == EMERGE_CANCELLED);
		eth->m_apply_result_action = action;
		eth->m_apply_result_block = block;
		if (action == EMERGE_GENERATED) {
			eth->m_apply_result_bm = transferBlockMakeDataToHeap(&bmdata);
			assert(eth->m_apply_result_bm != nullptr);
		} else {
			assert(eth->m_apply_result_bm == nullptr);
		}
		eth->m_apply_done.signal();
		return;
	}

	assert(item.bm);
	std::map<v3s16, MapBlock *> modified;
	MapBlock *block = nullptr;
	{
		Server::EnvAutoLock envlock(server);
		ServerMap &smap = server->m_env->getServerMap();
		{
			ScopeProfiler sp_fb(g_profiler,
				"Server: emerge apply: finishBlockMake", SPT_AVG, PRECISION_MICRO);
			smap.finishBlockMake(item.bm.get(), &modified, server->m_env);
		}
		block = smap.getBlockNoCreateNoEx(item.pos);
		if (!block) {
			errorstream << "EmergeManager::runServerApplyItem: Couldn't grab block we "
				"just generated: " << item.pos << std::endl;
		} else {
			v3s16 minp = item.bm->blockpos_min * MAP_BLOCKSIZE;
			v3s16 maxp = item.bm->blockpos_max * MAP_BLOCKSIZE +
				v3s16(1, 1, 1) * (MAP_BLOCKSIZE - 1);
			MapEditEventAreaIgnorer ign(
				&server->m_ignore_map_edit_events_area,
				VoxelArea(minp, maxp));
			(void)ign; // may be nulled if nested
			try {
				ScopeProfiler sp_env(g_profiler,
					"Server: emerge apply: environment_OnGenerated", SPT_AVG,
					PRECISION_MICRO);
				server->getScriptIface()->environment_OnGenerated(
					minp, maxp, item.blockseed);
			} catch (LuaError &e) {
				server->setAsyncFatalError(e);
			}
		}
		if (block)
			modified[item.pos] = block;
		if (!modified.empty()) {
			ScopeProfiler sp_dispatch(g_profiler,
				"Server: emerge apply: dispatchEvent", SPT_AVG, PRECISION_MICRO);
			MapEditEvent event;
			event.type = MEET_OTHER;
			event.setModifiedBlocks(modified);
			smap.dispatchEvent(event);
		}
	}
	// unique_ptr in item frees vmanip after this line
	eth->m_apply_result_action = EMERGE_GENERATED;
	eth->m_apply_result_block = block;
	eth->m_apply_done.signal();
}

void EmergeManager::processEmergeAppliesOnServerThread(Server *server)
{
	assert(server);
	// Keep main loop fairness: this queue now carries both generated apply and
	// FROM_DISK second-pass work. Draining to empty can starve map send / peers.
	for (u32 n = 0; n < SERVER_EMERGE_APPLY_BATCH; n++) {
		EmergeApplyItem item;
		{
			std::unique_lock<std::mutex> lk(m_apply_mutex);
			if (m_server_emerge_work_queue.empty())
				return;
			item = std::move(m_server_emerge_work_queue.front());
			m_server_emerge_work_queue.pop();
		}
		m_apply_space_cv.notify_all();
		runServerApplyItem(server, std::move(item));
	}
}

bool EmergeManager::queueGeneratedForServerApply(EmergeThread *eth, v3s16 pos, u64 blockseed,
		BlockMakeData *stack_bm)
{
	assert(eth);
	eth->m_apply_result_action = EMERGE_CANCELLED;
	eth->m_apply_result_block = nullptr;
	eth->m_apply_result_bm.reset();
	{
		std::unique_lock<std::mutex> lk(m_apply_mutex);
		while (m_server_emerge_work_queue.size() >= m_apply_queue_limit && !m_apply_stopping)
			m_apply_space_cv.wait(lk);
		if (m_apply_stopping)
			return false;
		EmergeApplyItem it;
		it.kind = EmergeApplyItem::APPLY_GENERATED;
		it.bm = transferBlockMakeDataToHeap(stack_bm);
		it.pos = pos;
		it.blockseed = blockseed;
		it.thread = eth;
		m_server_emerge_work_queue.push(std::move(it));
	}
	eth->m_apply_done.wait();
	// If m_apply_stopping, stopThreads() drained the queue and applied first;
	// the wait still completed with a valid result or nullptr as for a normal run.
	return true;
}

bool EmergeManager::queueFromDiskSecondPassForServer(
		EmergeThread *eth, v3s16 pos, bool allow_gen, std::string *from_db)
{
	assert(eth);
	assert(from_db);
	eth->m_apply_result_action = EMERGE_CANCELLED;
	eth->m_apply_result_block = nullptr;
	eth->m_apply_result_bm.reset();
	{
		std::unique_lock<std::mutex> lk(m_apply_mutex);
		while (m_server_emerge_work_queue.size() >= m_apply_queue_limit && !m_apply_stopping)
			m_apply_space_cv.wait(lk);
		if (m_apply_stopping)
			return false;
		EmergeApplyItem it;
		it.kind = EmergeApplyItem::FROM_DISK_SECOND_PASS;
		it.pos = pos;
		it.allow_gen = allow_gen;
		it.thread = eth;
		it.from_db.swap(*from_db);
		m_server_emerge_work_queue.push(std::move(it));
	}
	eth->m_apply_done.wait();
	return true;
}

void EmergeManager::serverPrepareEmergeFirstPass(Server *server)
{
	assert(server);
	for (u32 n = 0; n < SERVER_EMERGE_APPLY_BATCH; n++) {
		// Backpressure: without this, server prep can run far ahead of emerge and
		// hold arbitrarily many heap BlockMakeData in m_prepared_first (stress tests).
		{
			std::lock_guard<std::mutex> lk(m_prep_mutex);
			if (m_prepared_first.size() >= m_prep_inflight_limit)
				return;
		}

		v3s16 pos;
		BlockEmergeData bedata;
		bool have_bedata = false;

		{
			MutexAutoLock qlock(m_queue_mutex);
			if (m_server_prep_inbox.empty())
				return;

			pos = m_server_prep_inbox.front();
			auto itb = m_blocks_enqueued.find(pos);
			if (itb == m_blocks_enqueued.end()) {
				// Emerge thread already popped this row; unblock waiters without a
				// duplicate envlock peek (they still hold bedata locally).
				m_server_prep_inbox.pop();
				have_bedata = false;
			} else {
				bedata = itb->second;
				have_bedata = true;
				m_server_prep_inbox.pop();
			}
		}

		if (!have_bedata) {
			EmergePreparedFirstPass skip_prep;
			skip_prep.fallthrough_to_emerge_envlock = true;
			std::lock_guard<std::mutex> pl(m_prep_mutex);
			m_prepared_first[pos] = std::move(skip_prep);
			m_prep_cv.notify_all();
			continue;
		}

		const bool allow_gen = (bedata.flags & BLOCK_EMERGE_ALLOW_GEN) != 0;
		MapBlock *b = nullptr;
		BlockMakeData bmd;
		EmergeAction a;
		{
			Server::EnvAutoLock el(server);
			ScopeProfiler sp(g_profiler,
					"Server: emerge prep: getBlockOrStartGen", SPT_AVG,
					PRECISION_MICRO);
			a = emerge_map_get_block_or_start_gen(
					&server->m_env->getServerMap(), pos, allow_gen, nullptr, &b, &bmd);
		}
		EmergePreparedFirstPass prep;
		prep.action = a;
		prep.block = b;
		if (a == EMERGE_GENERATED)
			prep.gen_bm = transferBlockMakeDataToHeap(&bmd);
		{
			std::lock_guard<std::mutex> pl(m_prep_mutex);
			m_prepared_first[pos] = std::move(prep);
		}
		m_prep_cv.notify_all();
	}
}

bool EmergeManager::tryWaitTakePreparedFirstPass(
		v3s16 pos, EmergePreparedFirstPass *out)
{
	assert(out);
	std::unique_lock<std::mutex> lk(m_prep_mutex);
	auto it = m_prepared_first.find(pos);
	if (it != m_prepared_first.end()) {
		*out = std::move(it->second);
		m_prepared_first.erase(it);
		return true;
	}
	if (m_prep_stopping)
		return false;

	const auto deadline = std::chrono::steady_clock::now() + PREP_WAIT_TIME_LIMIT;

	while (m_prepared_first.find(pos) == m_prepared_first.end() && !m_prep_stopping) {
		if (m_prep_cv.wait_until(lk, deadline) == std::cv_status::timeout)
			break;
	}
	if (m_prep_stopping)
		return false;
	it = m_prepared_first.find(pos);
	if (it == m_prepared_first.end()) {
		warningstream << "Emerge: prep wait timeout " << pos << std::endl;
		return false;
	}
	*out = std::move(it->second);
	m_prepared_first.erase(it);
	return true;
}

////
//// EmergeThread
////

EmergeThread::EmergeThread(Server *server, int ethreadid) :
	enable_mapgen_debug_info(false),
	id(ethreadid),
	m_server(server),
	m_map(nullptr),
	m_emerge(nullptr),
	m_mapgen(nullptr),
	m_trans_liquid(nullptr)
{
	m_name = "Emerge-" + itos(ethreadid);
}


void EmergeThread::signal()
{
	m_queue_event.signal();
}


bool EmergeThread::pushBlock(v3s16 pos)
{
	m_block_queue.push(pos);
	return true;
}


void EmergeThread::cancelPendingItems()
{
	MutexAutoLock queuelock(m_emerge->m_queue_mutex);

	while (!m_block_queue.empty()) {
		BlockEmergeData bedata;
		v3s16 pos;

		pos = m_block_queue.front();
		m_block_queue.pop();

		m_emerge->popBlockEmergeData(pos, &bedata);

		runCompletionCallbacks(pos, EMERGE_CANCELLED, bedata.callbacks);
	}
}


void EmergeThread::runCompletionCallbacks(v3s16 pos, EmergeAction action,
	const EmergeCallbackList &callbacks)
{
	m_emerge->reportCompletedEmerge(action);

	for (size_t i = 0; i != callbacks.size(); i++) {
		EmergeCompletionCallback callback;
		void *param;

		callback = callbacks[i].first;
		param    = callbacks[i].second;

		callback(pos, action, param);
	}
}


bool EmergeThread::popBlockEmerge(v3s16 *pos, BlockEmergeData *bedata)
{
	MutexAutoLock queuelock(m_emerge->m_queue_mutex);

	if (m_block_queue.empty())
		return false;

	*pos = m_block_queue.front();
	m_block_queue.pop();

	m_emerge->popBlockEmergeData(*pos, bedata);

	return true;
}


EmergeAction EmergeThread::getBlockOrStartGenImpl(const v3s16 pos, bool allow_gen,
	const std::string *from_db, MapBlock **block, BlockMakeData *bmdata)
{
	return emerge_map_get_block_or_start_gen(
			m_map, pos, allow_gen, from_db, block, bmdata);
}

EmergeAction EmergeThread::getBlockOrStartGen(const v3s16 pos, bool allow_gen,
	 const std::string *from_db, MapBlock **block, BlockMakeData *bmdata)
{
	Server::EnvAutoLock envlock(m_server);
	ScopeProfiler sp(g_profiler,
		"EmergeThread: getBlockOrStartGen", SPT_AVG, PRECISION_MICRO);
	return getBlockOrStartGenImpl(pos, allow_gen, from_db, block, bmdata);
}


bool EmergeThread::initScripting()
{
	m_script = std::make_unique<EmergeScripting>(this);

	try {
		m_script->loadMod(Server::getBuiltinLuaPath() + DIR_DELIM + "init.lua",
			BUILTIN_MOD_NAME);
		m_script->checkSetByBuiltin();
	} catch (const ModError &e) {
		errorstream << "Execution of mapgen base environment failed." << std::endl;
		m_server->setAsyncFatalError(e.what());
		return false;
	}

	const auto &list = m_server->m_mapgen_init_files;
	try {
		for (auto &it : list)
			m_script->loadMod(it.second, it.first);

		m_script->on_mods_loaded();
	} catch (const ModError &e) {
		errorstream << "Failed to load mod script inside mapgen environment." << std::endl;
		m_server->setAsyncFatalError(e.what());
		return false;
	}

	return true;
}


void *EmergeThread::run()
{
	BEGIN_DEBUG_EXCEPTION_HANDLER

	v3s16 pos;
	std::map<v3s16, MapBlock*> modified_blocks;
	std::string databuf;

	m_map    = &m_server->m_env->getServerMap();
	m_emerge = m_server->getEmergeManager();
	m_mapgen = m_emerge->m_mapgens[id];
	enable_mapgen_debug_info = m_emerge->enable_mapgen_debug_info;

	if (!initScripting()) {
		m_script.reset();
		stop(); // do not enter main loop
	}

	try {
	while (!stopRequested()) {
		BlockEmergeData bedata;
		BlockMakeData bmdata;
		EmergeAction action;
		MapBlock *block = nullptr;

		porting::TriggerMemoryTrim();

		if (!popBlockEmerge(&pos, &bedata)) {
			m_queue_event.wait();
			continue;
		}

		g_profiler->add(m_name + ": processed [#]", 1);

		if (blockpos_over_max_limit(pos))
			continue;

		bool allow_gen = bedata.flags & BLOCK_EMERGE_ALLOW_GEN;

		EmergePreparedFirstPass prep;
		bool used_server_prep = m_emerge->tryWaitTakePreparedFirstPass(pos, &prep);
		if (used_server_prep && prep.fallthrough_to_emerge_envlock)
			used_server_prep = false;
		if (used_server_prep) {
			action = prep.action;
			block = prep.block;
			if (action == EMERGE_GENERATED) {
				if (prep.gen_bm) {
					transferBlockMakeDataFromUniqueToStack(
							std::move(prep.gen_bm), &bmdata);
				} else {
					used_server_prep = false;
				}
			}
		}
		if (!used_server_prep)
			action = getBlockOrStartGen(pos, allow_gen, nullptr, &block, &bmdata);

		/* Try to load it */
		if (action == EMERGE_FROM_DISK) {
			auto &m_db = *m_emerge->m_db;
			{
				ScopeProfiler sp(g_profiler, "EmergeThread: load block - async (sum)");
				MutexAutoLock slock(m_db.stripeMutex(pos));
				// Note: this can throw an exception, but there isn't really
				// a good, safe way to handle it.
				m_db.loadBlock(pos, databuf);
			}
			// Non-empty blobs are installed on the server thread; empty + allow_gen
			// runs emerge_map_get_block_or_start_gen here under EnvAutoLock; empty
			// + !allow_gen cancels without queueing for a server-step second pass.
			if (databuf.empty() && !allow_gen) {
				action = EMERGE_CANCELLED;
				block = nullptr;
			} else if (databuf.empty() && allow_gen) {
				Server::EnvAutoLock envlock(m_server);
				ScopeProfiler sp(g_profiler,
						"EmergeThread: from_disk second pass (local)", SPT_AVG,
						PRECISION_MICRO);
				action = emerge_map_get_block_or_start_gen(
						m_map, pos, allow_gen, &databuf, &block, &bmdata);
			} else if (!m_emerge->queueFromDiskSecondPassForServer(
						this, pos, allow_gen, &databuf)) {
				action = EMERGE_CANCELLED;
				block = nullptr;
			} else {
				action = m_apply_result_action;
				block = m_apply_result_block;
				m_apply_result_block = nullptr;
				if (action == EMERGE_GENERATED) {
					assert(m_apply_result_bm);
					transferBlockMakeDataFromUniqueToStack(
							std::move(m_apply_result_bm), &bmdata);
				}
				assert(!m_apply_result_bm);
			}
			databuf.clear();
		}

		bool did_server_apply = false;
		/* Generate it */
		if (action == EMERGE_GENERATED) {
			bool error = false;
			m_trans_liquid = &bmdata.transforming_liquid;

			{
				ScopeProfiler sp(g_profiler,
					"EmergeThread: Mapgen::makeChunk", SPT_AVG);

				m_mapgen->makeChunk(&bmdata);
			}

			{
				ScopeProfiler sp(g_profiler,
					"EmergeThread: Lua on_generated", SPT_AVG);

				try {
					m_script->on_generated(&bmdata, m_mapgen->blockseed);
				} catch (const LuaError &e) {
					m_server->setAsyncFatalError(e);
					error = true;
				}
			}

			if (!error) {
				if (!m_emerge->queueGeneratedForServerApply(
							this, pos, m_mapgen->blockseed, &bmdata)) {
					m_map->cancelBlockMake(&bmdata);
					error = true;
				} else {
					block = m_apply_result_block;
					m_apply_result_block = nullptr;
					assert(!m_mapgen->generating);
					m_mapgen->gennotify.clearEvents();
					m_mapgen->vm = nullptr;
					did_server_apply = true;
				}
			} else
				m_map->cancelBlockMake(&bmdata);
			if (!block || error)
				action = EMERGE_ERRORED;

			m_trans_liquid = nullptr;
		}

		// Lua emerge_area callbacks (see LuaEmergeAreaCallback) take EnvAutoLock.
		// When did_server_apply, finishBlockMake / server environment_OnGenerated /
		// dispatch already ran in runServerApplyItem before we returned here.
		runCompletionCallbacks(pos, action, bedata.callbacks);

		if (block && !did_server_apply)
			modified_blocks[pos] = block;

		if (!modified_blocks.empty() && !did_server_apply) {
			MapEditEvent event;
			event.type = MEET_OTHER;
			event.setModifiedBlocks(modified_blocks);
			Server::EnvAutoLock envlock(m_server);
			ScopeProfiler sp_dispatch(g_profiler,
				"EmergeThread: dispatchEvent", SPT_AVG, PRECISION_MICRO);
			m_map->dispatchEvent(event);
		}
		modified_blocks.clear();
	}
	} catch (VersionMismatchException &e) {
		std::ostringstream err;
		err << "World data version mismatch in MapBlock " << pos << std::endl
			<< "----" << std::endl
			<< "\"" << e.what() << "\"" << std::endl
			<< "See debug.txt." << std::endl
			<< "World probably saved by a newer version of " PROJECT_NAME_C "."
			<< std::endl;
		m_server->setAsyncFatalError(err.str());
	} catch (SerializationError &e) {
		std::ostringstream err;
		err << "Invalid data in MapBlock " << pos << std::endl
			<< "----" << std::endl
			<< "\"" << e.what() << "\"" << std::endl
			<< "See debug.txt." << std::endl
			<< "This can be ignored using the `ignore_world_load_errors` setting. "
			<< "But it will also destroy stuff in the affected MapBlocks, do not use."
			<< std::endl;
		m_server->setAsyncFatalError(err.str());
	}

	try {
		if (m_script)
			m_script->on_shutdown();
	} catch (const ModError &e) {
		m_server->setAsyncFatalError(e.what());
	}

	cancelPendingItems();

	END_DEBUG_EXCEPTION_HANDLER
	return NULL;
}
