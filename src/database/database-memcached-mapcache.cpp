// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2025 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "config.h"

#if USE_MEMCACHED

#include "database-memcached-mapcache.h"

#include "debug.h"
#include "filesys.h"
#include "log.h"
#include "util/hashing.h"
#include "util/hex.h"
#include "util/string.h"

#include <libmemcached/memcached.h>
#include <cstdlib>
#include <string_view>

namespace
{

// Fixed TTL for cached map blocks (seconds). Not configurable.
constexpr time_t MAP_MEMCACHED_TTL_S = 86400;

static memcached_st *create_memcached_handle(const std::string &connection_string)
{
	memcached_st *mc = memcached_create(nullptr);
	if (!mc)
		return nullptr;

	bool any = false;
	for (const std::string &spec : str_split(connection_string, ',')) {
		std::string_view part = trim(spec);
		if (part.empty())
			continue;
		const std::string s(part);
		const size_t colon = s.rfind(':');
		if (colon == std::string::npos || colon + 1 >= s.size()) {
			warningstream << "MapDatabaseMemcachedCache: invalid server "
					     "\""
				      << s
				      << "\" (expected host:port), skipping."
				      << std::endl;
			continue;
		}
		const std::string host = std::string(trim(std::string_view(s.data(), colon)));
		const int port = atoi(s.c_str() + colon + 1);
		if (port <= 0 || port > 65535) {
			warningstream << "MapDatabaseMemcachedCache: invalid port in \""
				      << s << "\", skipping." << std::endl;
			continue;
		}
		memcached_return_t rc =
				memcached_server_add(mc, host.c_str(), static_cast<in_port_t>(port));
		if (rc != MEMCACHED_SUCCESS) {
			warningstream << "MapDatabaseMemcachedCache: memcached_server_add(\""
				      << s << "\") failed: " << memcached_strerror(mc, rc)
				      << std::endl;
			memcached_free(mc);
			return nullptr;
		}
		any = true;
	}

	if (!any) {
		memcached_free(mc);
		return nullptr;
	}

	return mc;
}

static std::string make_namespace_prefix(
		const std::string &savedir, const std::string &override_ns)
{
	if (!override_ns.empty())
		return override_ns;
	return hex_encode(hashing::sha256(fs::AbsolutePath(savedir)));
}

} // namespace

MapDatabaseMemcachedCache::MapDatabaseMemcachedCache(std::unique_ptr<MapDatabase> inner,
		memcached_st *memc,
		const std::string &key_prefix) :
	m_inner(std::move(inner)),
	m_memc(memc),
	m_key_prefix(key_prefix)
{
	sanity_check(m_inner.get());
	sanity_check(m_memc);
}

MapDatabaseMemcachedCache::~MapDatabaseMemcachedCache()
{
	memcached_free(m_memc);
}

void MapDatabaseMemcachedCache::beginSave()
{
	m_inner->beginSave();
}

void MapDatabaseMemcachedCache::endSave()
{
	m_inner->endSave();
}

void MapDatabaseMemcachedCache::verifyDatabase()
{
	m_inner->verifyDatabase();
}

std::string MapDatabaseMemcachedCache::makeKey(const v3s16 &pos) const
{
	return m_key_prefix + ":" + std::to_string(getBlockAsInteger(pos));
}

void MapDatabaseMemcachedCache::logMemcachedError(const char *what, memcached_return_t err)
{
	warningstream << "MapDatabaseMemcachedCache: " << what << ": "
		      << memcached_strerror(m_memc, err) << std::endl;
}

void MapDatabaseMemcachedCache::cacheSet(const std::string &key, std::string_view data)
{
	memcached_return_t rc = memcached_set(m_memc, key.data(), key.size(), data.data(),
			data.size(), MAP_MEMCACHED_TTL_S, static_cast<uint32_t>(0));
	if (rc != MEMCACHED_SUCCESS) {
		// Item too large, transient errors, etc.: degrade to DB-only for this block.
		logMemcachedError("memcached_set", rc);
	}
}

bool MapDatabaseMemcachedCache::saveBlock(const v3s16 &pos, std::string_view data)
{
	if (!m_inner->saveBlock(pos, data))
		return false;
	cacheSet(makeKey(pos), data);
	return true;
}

void MapDatabaseMemcachedCache::loadBlock(const v3s16 &pos, std::string *block)
{
	block->clear();
	const std::string key = makeKey(pos);
	size_t value_length = 0;
	uint32_t flags = 0;
	memcached_return_t err = MEMCACHED_SUCCESS;
	char *value = memcached_get(m_memc, key.data(), key.size(), &value_length, &flags, &err);

	if (value && err == MEMCACHED_SUCCESS && value_length > 0) {
		block->assign(value, value_length);
		free(value);
		return;
	}
	if (value)
		free(value);

	m_inner->loadBlock(pos, block);
	if (!block->empty())
		cacheSet(key, *block);
}

bool MapDatabaseMemcachedCache::deleteBlock(const v3s16 &pos)
{
	const std::string key = makeKey(pos);
	memcached_return_t rc = memcached_delete(m_memc, key.data(), key.size(), 0);
	if (rc != MEMCACHED_SUCCESS && rc != MEMCACHED_NOTFOUND)
		logMemcachedError("memcached_delete", rc);

	return m_inner->deleteBlock(pos);
}

void MapDatabaseMemcachedCache::listAllLoadableBlocks(std::vector<v3s16> &dst)
{
	m_inner->listAllLoadableBlocks(dst);
}

MapDatabase *tryWrapMapDatabaseMemcached(MapDatabase *inner,
		const std::string &savedir_for_namespace,
		const std::string &connection_string,
		const std::string &namespace_override)
{
	if (!inner || connection_string.empty())
		return inner;

	memcached_st *mc = create_memcached_handle(connection_string);
	if (!mc) {
		warningstream << "MapDatabaseMemcachedCache: no usable memcached servers; "
				     "using map database without Memcached layer."
			      << std::endl;
		return inner;
	}

	const std::string ns = make_namespace_prefix(savedir_for_namespace, namespace_override);
	infostream << "MapDatabaseMemcachedCache: enabled with namespace prefix length "
		   << ns.size() << std::endl;

	return new MapDatabaseMemcachedCache(std::unique_ptr<MapDatabase>(inner), mc, ns);
}

#endif // USE_MEMCACHED
