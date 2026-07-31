#pragma once

#include "cuac/runtime/execution.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace cuac_test {

static const char ACCEPTED_LIVE_SQL[] = "SELECT id, login, site_admin FROM cuac_scan(connector := 'github', "
                                        "relation := 'duckdb_login_search_page') ORDER BY id";

enum class QueryRuntimeScenario {
	SUCCESS,
	STREAMING,
	BLOCKING,
	BLOCKING_ANONYMOUS_ONLY,
	OPEN_EXECUTION_CANCELLED,
	OPEN_INTERNAL_ERROR,
	OPEN_POLICY_ERROR,
	OPEN_RESOURCE_ERROR,
	OPEN_LOCAL_ADMISSION_ERROR,
	OPEN_UNKNOWN_EXCEPTION,
	TRANSPORT_ERROR,
	HTTP_STATUS_ERROR,
	DECODE_ERROR,
	SCHEMA_ERROR,
	POLICY_ERROR,
	RESOURCE_ERROR,
	AUTHENTICATION_ERROR,
	AUTHORIZATION_ERROR,
	REMOTE_PROTOCOL_ERROR,
	INTERNAL_ERROR,
	UNKNOWN_ERROR,
	NULL_STREAM,
	EMPTY_BATCH,
	ROWS_WITH_FALSE,
	LATE_RESOURCE_ERROR_ONCE,
	LATE_LOCAL_ADMISSION_ERROR_ONCE,
	MISALIGNED_BATCH,
	OVERSIZED_BATCH,
	GRAPHQL_SUCCESS
};

struct QueryLifecycleProbe {
	std::atomic<uint64_t> anonymous_open_calls;
	std::atomic<uint64_t> authorization_open_calls;
	std::atomic<uint64_t> anonymous_authorizations;
	std::atomic<uint64_t> github_bearer_authorizations;
	std::atomic<uint64_t> streams_opened;
	std::atomic<uint64_t> next_calls;
	std::atomic<uint64_t> batches;
	std::atomic<uint64_t> rows;
	std::atomic<uint64_t> cancellations;
	std::atomic<uint64_t> streams_closed;
	std::atomic<uint64_t> active_waiters;
	std::atomic<bool> late_failure_enabled;
	std::mutex mutex;
	std::condition_variable condition;

	QueryLifecycleProbe();
};

std::shared_ptr<const cuac::ScanExecutor> BuildQueryScenarioExecutor(QueryRuntimeScenario scenario,
                                                                     std::shared_ptr<QueryLifecycleProbe> probe);

} // namespace cuac_test
