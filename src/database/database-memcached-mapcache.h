// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2025 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "config.h"

#if USE_MEMCACHED

#include <memory>
#include <string>
#include "database.h"

struct memcached_st;

class MapDatabaseMemcachedCache : public MapDatabase
{
public:
	MapDatabaseMemcachedCache(std::unique_ptr<MapDatabase> inner,
			memcached_st *memc,
			const std::string &key_prefix);
	~MapDatabaseMemcachedCache() override;

	void beginSave() override;
	void endSave() override;
	void verifyDatabase() override;

	bool saveBlock(const v3s16 &pos, std::string_view data) override;
	void loadBlock(const v3s16 &pos, std::string *block) override;
	bool deleteBlock(const v3s16 &pos) override;
	void listAllLoadableBlocks(std::vector<v3s16> &dst) override;

private:
	std::string makeKey(const v3s16 &pos) const;
	void cacheSet(const std::string &key, std::string_view data);
	void logMemcachedError(const char *what, memcached_return_t err);

	std::unique_ptr<MapDatabase> m_inner;
	memcached_st *m_memc;
	std::string m_key_prefix;
};

/**
 * Wraps @p inner with a Memcached read-through/write-through block cache when
 * @p connection_string is non-empty and at least one server could be added.
 * On failure, logs a warning and returns @p inner unchanged.
 */
MapDatabase *tryWrapMapDatabaseMemcached(MapDatabase *inner,
		const std::string &savedir_for_namespace,
		const std::string &connection_string,
		const std::string &namespace_override);

#endif // USE_MEMCACHED
