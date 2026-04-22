// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 kwolekr, Ryan Kwolek <kwolekr@minetest.net>

#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include "network/networkprotocol.h"
#include "irr_v3d.h"
#include "util/metricsbackend.h"
#include "mapgen/mapgen.h" // for MapgenParams
#include "map.h"

#define BLOCK_EMERGE_ALLOW_GEN   (1 << 0)
#define BLOCK_EMERGE_FORCE_QUEUE (1 << 1)

#define EMERGE_DBG_OUT(x) {                            \
	if (enable_mapgen_debug_info)                      \
		infostream << "EmergeThread: " x << std::endl; \
}

class EmergeManager;
class EmergeThread;
class NodeDefManager;
class MapSettingsManager;
class BiomeManager;
class OreManager;
class DecorationManager;
class SchematicManager;
class Server;
class ModApiMapgen;
struct MapDatabaseAccessor;

// Structure containing inputs/outputs for chunk generation
struct BlockMakeData {
	MMVManip *vmanip = nullptr;
	// Global map seed
	u64 seed = 0;
	v3s16 blockpos_min;
	v3s16 blockpos_max;
	UniqueQueue<v3s16> transforming_liquid;
	const NodeDefManager *nodedef = nullptr;

	BlockMakeData() = default;

	~BlockMakeData() { delete vmanip; }
};

// Result from processing an item on the emerge queue
enum EmergeAction {
	EMERGE_CANCELLED,
	EMERGE_ERRORED,
	EMERGE_FROM_MEMORY,
	EMERGE_FROM_DISK,
	EMERGE_GENERATED,
};

constexpr const char *emergeActionStrs[] = {
	"cancelled",
	"errored",
	"from_memory",
	"from_disk",
	"generated",
};

// Callback
typedef void (*EmergeCompletionCallback)(
	v3s16 blockpos, EmergeAction action, void *param);

typedef std::vector<
	std::pair<
		EmergeCompletionCallback,
		void *
	>
> EmergeCallbackList;

struct BlockEmergeData {
	u16 peer_requested;
	u16 flags;
	EmergeCallbackList callbacks;
};

/// @brief Work deferred from an EmergeThread; applied on the server thread
/// under EnvAutoLock.
struct EmergeApplyItem {
	enum Kind : u8 {
		APPLY_GENERATED,
		FROM_DISK_SECOND_PASS,
	};

	Kind kind = APPLY_GENERATED;
	v3s16 pos{0, 0, 0};
	bool allow_gen = false;
	EmergeThread *thread = nullptr;

	// APPLY_GENERATED payload
	u64 blockseed = 0;
	std::unique_ptr<BlockMakeData> bm;

	// FROM_DISK_SECOND_PASS payload
	std::string from_db;
};

class MapBlock;

/// @brief First pass of getBlockOrStartGen, prepared on the server thread.
struct EmergePreparedFirstPass {
	EmergeAction action = EMERGE_CANCELLED;
	MapBlock *block = nullptr;
	std::unique_ptr<BlockMakeData> gen_bm;
	/// Stale prep inbox entry: emerge already popped m_blocks_enqueued; run
	/// getBlockOrStartGen on the emerge thread.
	bool fallthrough_to_emerge_envlock = false;
};

class EmergeParams {
	friend class EmergeManager;
public:
	EmergeParams() = delete;
	~EmergeParams();
	DISABLE_CLASS_COPY(EmergeParams);

	const NodeDefManager *ndef; // shared
	bool enable_mapgen_debug_info;

	u32 gen_notify_on;
	const std::set<u32> *gen_notify_on_deco_ids; // shared
	const std::set<std::string> *gen_notify_on_custom; // shared

	BiomeGen *biomegen;
	BiomeManager *biomemgr;
	OreManager *oremgr;
	DecorationManager *decomgr;
	SchematicManager *schemmgr;

	inline GenerateNotifier createNotifier() const {
		return GenerateNotifier(gen_notify_on, gen_notify_on_deco_ids,
			gen_notify_on_custom);
	}

private:
	EmergeParams(EmergeManager *parent, const BiomeGen *biomegen,
		const BiomeManager *biomemgr,
		const OreManager *oremgr, const DecorationManager *decomgr,
		const SchematicManager *schemmgr);
};

class EmergeManager {
	/* The mod API needs unchecked access to allow:
	 * - using decomgr or oremgr to place decos/ores
	 * - using schemmgr to load and place schematics
	 */
	friend class ModApiMapgen;
public:
	const NodeDefManager *ndef;
	bool enable_mapgen_debug_info;

	// Generation Notify
	u32 gen_notify_on = 0;
	std::set<u32> gen_notify_on_deco_ids;
	std::set<std::string> gen_notify_on_custom;

	// Parameters passed to mapgens owned by ServerMap
	// TODO(hmmmm): Remove this after mapgen helper methods using them
	// are moved to ServerMap
	MapgenParams *mgparams;

	// Hackish workaround:
	// For now, EmergeManager must hold onto a ptr to the Map's setting manager
	// since the Map can only be accessed through the Environment, and the
	// Environment is not created until after script initialization.
	MapSettingsManager *map_settings_mgr;

	// Methods
	EmergeManager(Server *server, MetricsBackend *mb);
	~EmergeManager();
	DISABLE_CLASS_COPY(EmergeManager);

	const BiomeGen *getBiomeGen() const { return biomegen; }

	// no usage restrictions
	const BiomeManager *getBiomeManager() const { return biomemgr; }
	const OreManager *getOreManager() const { return oremgr; }
	const DecorationManager *getDecorationManager() const { return decomgr; }
	const SchematicManager *getSchematicManager() const { return schemmgr; }
	// only usable before mapgen init
	BiomeManager *getWritableBiomeManager();
	OreManager *getWritableOreManager();
	DecorationManager *getWritableDecorationManager();
	SchematicManager *getWritableSchematicManager();

	void initMapgens(MapgenParams *mgparams);
	/// @param holder non-owned reference that must stay alive
	void initMap(MapDatabaseAccessor *holder);
	/// resets the reference
	void resetMap();

	void startThreads();
	void stopThreads();

	bool enqueueBlockEmerge(
		session_t peer_id,
		v3s16 blockpos,
		bool allow_generate,
		bool ignore_queue_limits=false);

	bool enqueueBlockEmergeEx(
		v3s16 blockpos,
		session_t peer_id,
		u16 flags,
		EmergeCompletionCallback callback,
		void *callback_param);

	size_t getQueueSize();
	bool isBlockInQueue(v3s16 pos);

	Mapgen *getCurrentMapgen();

	// Mapgen helpers methods
	int getSpawnLevelAtPoint(v2s16 p);
	bool isBlockUnderground(v3s16 blockpos);

	/// @return min edge of chunk in block units
	static v3s16 getContainingChunk(v3s16 blockpos, v3s16 chunksize);

	/**
	 * Drain the server emerge work queue (generated apply + disk second pass).
	 * Must be called from the server main thread (e.g. from Server::AsyncRunStep).
	 */
	void processEmergeAppliesOnServerThread(Server *server);

	/**
	 * After makeChunk and emerge on_generated, transfer BlockMakeData to
	 * the apply queue, wait for the server to finish finishBlockMake +
	 * environment_OnGenerated + dispatch, then the caller runs mapgen
	 * post-apply cleanup.
	 * @return false on shutdown; caller must cancel the block make.
	 */
	bool queueGeneratedForServerApply(
			EmergeThread *eth, v3s16 pos, u64 blockseed, BlockMakeData *stack_bm);

	/**
	 * Run the second getBlockOrStartGen pass (after DB blob load) on the server
	 * thread under EnvAutoLock; publish action/block/bmdata back to @a eth.
	 * @return false on shutdown; caller should cancel this emerge item.
	 */
	bool queueFromDiskSecondPassForServer(
			EmergeThread *eth, v3s16 pos, bool allow_gen, std::string *from_db);

	/**
	 * Under EnvAutoLock, run the same map peek / init as the first
	 * getBlockOrStartGen call; store per @a pos for @ref tryWaitTakePreparedFirstPass.
	 * Call from the server main thread (e.g. before @ref processEmergeAppliesOnServerThread).
	 */
	void serverPrepareEmergeFirstPass(Server *server);

	/**
	 * Block until the server publishes @ref EmergePreparedFirstPass for @a pos
	 * (including fallthrough sentinel rows when emerge already popped the enqueue
	 * slot), or return false on shutdown / circuit breaker timeout (caller uses
	 * @ref getBlockOrStartGen on the emerge thread).
	 */
	bool tryWaitTakePreparedFirstPass(v3s16 pos, EmergePreparedFirstPass *out);

private:
	void initThreads(bool should_multithread);

	std::vector<Mapgen *> m_mapgens;
	std::vector<EmergeThread *> m_threads;
	bool m_threads_active = false;

	// Server reference
	Server *m_server = nullptr;
	// The map database
	MapDatabaseAccessor *m_db = nullptr;

	std::mutex m_queue_mutex;
	std::map<v3s16, BlockEmergeData> m_blocks_enqueued;
	std::unordered_map<u16, u32> m_peer_queue_count;

	u32 m_qlimit_total;
	u32 m_qlimit_diskonly;
	u32 m_qlimit_generate;

	// Emerge metrics
	MetricCounterPtr m_completed_emerge_counter[5];

	// Managers of various map generation-related components
	// Note that each Mapgen gets a copy(!) of these to work with
	BiomeGen *biomegen;
	BiomeManager *biomemgr;
	OreManager *oremgr;
	DecorationManager *decomgr;
	SchematicManager *schemmgr;

	// Requires m_queue_mutex held
	EmergeThread *getOptimalThread();

	bool pushBlockEmergeData(
		v3s16 pos,
		u16 peer_requested,
		u16 flags,
		EmergeCompletionCallback callback,
		void *callback_param,
		bool *entry_already_exists);

	bool popBlockEmergeData(v3s16 pos, BlockEmergeData *bedata);

	void reportCompletedEmerge(EmergeAction action);

	void runServerApplyItem(Server *server, EmergeApplyItem &&item);

	// Server-thread work from emerge threads: (1) generated-chunk apply —
	// finishBlockMake + environment_OnGenerated + dispatch; (2) FROM_DISK second
	// pass — emerge_map_get_block_or_start_gen with blob. One bounded queue so
	// apply and disk second pass share the same backpressure limit.
	// Profiler: "Server: emerge apply:" vs "Server: emerge disk:" — same drain, EmergeApplyItem::Kind.
	std::mutex m_apply_mutex;
	std::condition_variable m_apply_space_cv;
	std::queue<EmergeApplyItem> m_server_emerge_work_queue;
	u32 m_apply_queue_limit = 0;
	bool m_apply_stopping = false;

	/// Max rows in m_prepared_first (set in initThreads from qlimits + thread count).
	u32 m_prep_inflight_limit = 0;

	// Server-thread first pass of getBlockOrStartGen (map peek / initBlockMake)
	std::mutex m_prep_mutex;
	std::condition_variable m_prep_cv;
	bool m_prep_stopping = false;
	std::queue<v3s16> m_server_prep_inbox;
	std::map<v3s16, EmergePreparedFirstPass> m_prepared_first;

	friend class EmergeThread;
};
