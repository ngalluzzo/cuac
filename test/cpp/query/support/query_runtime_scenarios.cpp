#include "query/support/query_runtime_scenarios.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace cuac_test {
namespace {

cuac::TypedRow Row(int64_t id, const std::string &login, bool site_admin) {
	cuac::TypedRow result;
	result.values.push_back(cuac::TypedValue::BigInt(id));
	result.values.push_back(cuac::TypedValue::Varchar(login));
	result.values.push_back(cuac::TypedValue::Boolean(site_admin));
	return result;
}

cuac::TypedRow GraphqlRow(const std::string &id, const std::string &full_name, const std::string &owner_login,
                          int64_t stars, cuac::TypedValue primary_language, bool private_repository, bool archived,
                          std::int64_t updated_at) {
	cuac::TypedRow result;
	result.values.push_back(cuac::TypedValue::Varchar(id));
	result.values.push_back(cuac::TypedValue::Varchar(full_name));
	result.values.push_back(cuac::TypedValue::Varchar(owner_login));
	result.values.push_back(cuac::TypedValue::BigInt(stars));
	result.values.push_back(std::move(primary_language));
	result.values.push_back(cuac::TypedValue::Boolean(private_repository));
	result.values.push_back(cuac::TypedValue::Boolean(archived));
	result.values.push_back(cuac::TypedValue::Timestamptz(updated_at));
	return result;
}

void ThrowScenarioError(QueryRuntimeScenario scenario) {
	switch (scenario) {
	case QueryRuntimeScenario::TRANSPORT_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "", "remote transport failed safely");
	case QueryRuntimeScenario::HTTP_STATUS_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::HTTP_STATUS, "", "remote service returned a non-success status");
	case QueryRuntimeScenario::DECODE_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::DECODE, "", "response is not valid JSON");
	case QueryRuntimeScenario::SCHEMA_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::SCHEMA, "id", "value cannot be converted to BIGINT");
	case QueryRuntimeScenario::POLICY_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "", "request is not authorized");
	case QueryRuntimeScenario::RESOURCE_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "", "response exceeds its byte budget");
	case QueryRuntimeScenario::AUTHENTICATION_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::AUTHENTICATION, "secret",
		                           "named credential could not authenticate");
	case QueryRuntimeScenario::AUTHORIZATION_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::AUTHORIZATION, "", "authenticated principal is not authorized");
	case QueryRuntimeScenario::REMOTE_PROTOCOL_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::REMOTE_PROTOCOL, "errors",
		                           "remote protocol response reported application errors");
	case QueryRuntimeScenario::INTERNAL_ERROR:
		throw cuac::ExecutionError(cuac::ErrorStage::INTERNAL, "", "top-secret-provider-detail");
	case QueryRuntimeScenario::UNKNOWN_ERROR:
		throw std::runtime_error("top-secret-unknown-provider-detail");
	default:
		return;
	}
}

class QueryScenarioStream : public cuac::BatchStream {
public:
	QueryScenarioStream(QueryRuntimeScenario scenario_p, bool authenticated_p, cuac::PlannedProtocol protocol_p,
	                    std::shared_ptr<QueryLifecycleProbe> probe_p)
	    : scenario(scenario_p), authenticated(authenticated_p), protocol(protocol_p), probe(std::move(probe_p)),
	      offset(0), exhausted(false), cancelled(false), closed(false) {
	}

	~QueryScenarioStream() noexcept override {
		Close();
	}

	bool Next(cuac::ExecutionControl &control, cuac::TypedBatch &batch) override {
		probe->next_calls.fetch_add(1, std::memory_order_relaxed);
		batch.Clear();
		if (closed) {
			return false;
		}
		const bool blocking =
		    scenario == QueryRuntimeScenario::BLOCKING || scenario == QueryRuntimeScenario::BLOCKING_ANONYMOUS_ONLY;
		if (blocking) {
			probe->active_waiters.fetch_add(1, std::memory_order_relaxed);
			probe->condition.notify_all();
			while (!cancelled.load(std::memory_order_relaxed) && !control.IsCancellationRequested()) {
				std::unique_lock<std::mutex> guard(probe->mutex);
				probe->condition.wait_for(guard, std::chrono::milliseconds(2));
			}
			probe->active_waiters.fetch_sub(1, std::memory_order_relaxed);
			throw cuac::ExecutionCancelled();
		}
		ThrowScenarioError(scenario);
		if (scenario == QueryRuntimeScenario::GRAPHQL_SUCCESS) {
			if (!authenticated || protocol != cuac::PlannedProtocol::GRAPHQL) {
				throw std::logic_error("GraphQL Query fixture received the wrong public provider alternatives");
			}
			batch.column_types = {cuac::ValueKind::VARCHAR, cuac::ValueKind::VARCHAR,    cuac::ValueKind::VARCHAR,
			                      cuac::ValueKind::BIGINT,  cuac::ValueKind::VARCHAR,    cuac::ValueKind::BOOLEAN,
			                      cuac::ValueKind::BOOLEAN, cuac::ValueKind::TIMESTAMPTZ};
			if (offset != 0) {
				exhausted = true;
				return false;
			}
			batch.rows.push_back(GraphqlRow("NODE-A", "fixture/zero", "fixture", 0,
			                                cuac::TypedValue::Null(cuac::ValueKind::VARCHAR), false, false,
			                                INT64_C(1784419200000000)));
			batch.rows.push_back(GraphqlRow("NODE-B", "fixture/typed", "fixture", 9, cuac::TypedValue::Varchar("C++"),
			                                true, true, INT64_C(1784332800000000)));
			offset = batch.rows.size();
			probe->batches.fetch_add(1, std::memory_order_relaxed);
			probe->rows.fetch_add(batch.rows.size(), std::memory_order_relaxed);
			return true;
		}

		batch.column_types = {cuac::ValueKind::BIGINT, cuac::ValueKind::VARCHAR, cuac::ValueKind::BOOLEAN};
		if (scenario == QueryRuntimeScenario::EMPTY_BATCH) {
			probe->batches.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if (scenario == QueryRuntimeScenario::ROWS_WITH_FALSE) {
			batch.rows.push_back(Row(8, "dropped", false));
			return false;
		}
		if (scenario == QueryRuntimeScenario::LATE_RESOURCE_ERROR_ONCE) {
			if (probe->late_failure_enabled.load(std::memory_order_acquire)) {
				throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "response_bytes",
				                           "response exceeds its byte budget");
			}
			batch.rows.push_back(Row(static_cast<int64_t>(offset + 7), "before-error", false));
			offset++;
			probe->batches.fetch_add(1, std::memory_order_relaxed);
			probe->rows.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if (scenario == QueryRuntimeScenario::LATE_LOCAL_ADMISSION_ERROR_ONCE) {
			if (probe->late_failure_enabled.load(std::memory_order_acquire)) {
				auto properties = cuac::LocalAdmissionFailureProperties(cuac::AdmissionReason::BUFFERED_ROWS_EXHAUSTED,
				                                                        cuac::AdmissionScope::BULKHEAD, 800, 800, 64,
				                                                        37, false, cuac::FailurePhase::REQUEST);
				properties.step = 2;
				properties.attempt = 1;
				properties.rows_exposed = 1;
				properties.exposure_state = cuac::ExposureState::EXPOSED;
				throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "admission",
				                           "local Runtime admission rejected decoded rows", properties);
			}
			batch.rows.push_back(Row(static_cast<int64_t>(offset + 17), "before-admission-error", false));
			offset++;
			probe->batches.fetch_add(1, std::memory_order_relaxed);
			probe->rows.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if (authenticated) {
			if (offset != 0) {
				exhausted = true;
				return false;
			}
			batch.rows.push_back(Row(42, "authenticated", true));
			offset = 1;
			probe->batches.fetch_add(1, std::memory_order_relaxed);
			probe->rows.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if (scenario == QueryRuntimeScenario::STREAMING) {
			const auto first_id = static_cast<int64_t>(offset + 1);
			batch.rows.push_back(Row(first_id, "stream", false));
			batch.rows.push_back(Row(first_id + 1, "stream", true));
			offset += 2;
			probe->batches.fetch_add(1, std::memory_order_relaxed);
			probe->rows.fetch_add(batch.rows.size(), std::memory_order_relaxed);
			return true;
		}
		if (scenario == QueryRuntimeScenario::MISALIGNED_BATCH) {
			batch.rows.push_back(Row(1, "duck", false));
			batch.rows[0].values.pop_back();
			return true;
		}
		if (scenario == QueryRuntimeScenario::OVERSIZED_BATCH) {
			batch.rows.push_back(Row(1, "duck", false));
			batch.rows.push_back(Row(2, "other", true));
			batch.rows.push_back(Row(3, "duckdb", true));
			return true;
		}
		if (offset == 0) {
			batch.rows.push_back(Row(1, "duck", false));
			batch.rows.push_back(Row(2, "other", true));
			offset = 2;
		} else if (offset == 2) {
			batch.rows.push_back(Row(3, "duckdb", true));
			offset = 3;
		} else {
			exhausted = true;
			return false;
		}
		probe->batches.fetch_add(1, std::memory_order_relaxed);
		probe->rows.fetch_add(batch.rows.size(), std::memory_order_relaxed);
		return true;
	}

	void Cancel() noexcept override {
		if (!cancelled.exchange(true, std::memory_order_relaxed)) {
			probe->cancellations.fetch_add(1, std::memory_order_relaxed);
		}
		probe->condition.notify_all();
	}

	void Close() noexcept override {
		if (!closed.exchange(true, std::memory_order_relaxed)) {
			probe->streams_closed.fetch_add(1, std::memory_order_relaxed);
		}
		probe->condition.notify_all();
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		cuac::ExecutionSnapshot result;
		result.outcome = cancelled.load(std::memory_order_relaxed) ? cuac::ScanOutcome::CANCELLED
		                 : exhausted                               ? cuac::ScanOutcome::SUCCEEDED
		                 : offset == 0                             ? cuac::ScanOutcome::NOT_STARTED
		                                                           : cuac::ScanOutcome::RUNNING;
		result.elapsed_milliseconds = offset == 0 ? 0 : 7;
		result.remote_requests = offset == 0 ? 0 : 1;
		result.aggregate_attempts = offset == 0 ? 0 : 1;
		result.current_step = offset == 0 ? 0 : 1;
		result.exposure_state = offset == 0 ? cuac::ExposureState::UNACCEPTED : cuac::ExposureState::EXPOSED;
		result.rows_decoded = static_cast<std::uint64_t>(offset);
		result.rows_returned = static_cast<std::uint64_t>(offset);
		result.response_header_bytes = offset == 0 ? 0 : 64;
		result.wire_response_bytes = offset == 0 ? 0 : 128;
		result.decompressed_response_bytes = offset == 0 ? 0 : 256;
		result.peak_decoded_memory_bytes = offset == 0 ? 0 : 512;
		return result;
	}

private:
	QueryRuntimeScenario scenario;
	const bool authenticated;
	const cuac::PlannedProtocol protocol;
	std::shared_ptr<QueryLifecycleProbe> probe;
	std::size_t offset;
	bool exhausted;
	std::atomic<bool> cancelled;
	std::atomic<bool> closed;
};

class QueryScenarioExecutor : public cuac::ScanExecutor {
public:
	QueryScenarioExecutor(QueryRuntimeScenario scenario_p, std::shared_ptr<QueryLifecycleProbe> probe_p)
	    : scenario(scenario_p), probe(std::move(probe_p)) {
	}

	std::unique_ptr<cuac::BatchStream> Open(const cuac::ScanPlan &plan,
	                                        cuac::ExecutionControl &control) const override {
		if (plan.Authentication() != cuac::FeatureState::DISABLED) {
			throw std::logic_error("anonymous execution received an authenticated scan plan");
		}
		return OpenAuthorizationEnvelope(plan, cuac::ScanAuthorization::Anonymous(), control);
	}

	void Close() const noexcept override {
	}

protected:
	std::unique_ptr<cuac::BatchStream> OpenCredentialProviderEnvelope(const cuac::ScanPlan &plan,
	                                                                  const cuac::CredentialProvider &provider,
	                                                                  cuac::ExecutionControl &control) const override {
		if (plan.Authentication() != cuac::FeatureState::ENABLED) {
			throw std::logic_error("credential provider received an anonymous scan plan");
		}
		if (plan.Operation().Protocol() == cuac::PlannedProtocol::REST &&
		    (plan.RelationName() != "authenticated_user" || plan.Domain() != cuac::BaseDomain::SUCCESSFUL_ROOT_OBJECT ||
		     plan.Operation().Rest().cardinality != cuac::PlannedCardinality::EXACTLY_ONE_ON_SUCCESS)) {
			throw std::logic_error("credential provider received the wrong REST plan profile");
		}
		if (plan.Operation().Protocol() == cuac::PlannedProtocol::GRAPHQL &&
		    (plan.RelationName() != "viewer_repository_metrics" ||
		     plan.Domain() != cuac::BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES ||
		     plan.Operation().Graphql().cardinality != cuac::PlannedCardinality::ZERO_TO_MANY)) {
			throw std::logic_error("credential provider received the wrong GraphQL plan profile");
		}
		auto resolved = ResolveCredentialWithAuthorityAfterAdmission(plan, provider, control);
		return OpenAuthorizationEnvelope(plan, std::move(resolved.authorization), control);
	}

	std::unique_ptr<cuac::BatchStream> OpenAuthorizationEnvelope(const cuac::ScanPlan &plan,
	                                                             cuac::ScanAuthorization authorization,
	                                                             cuac::ExecutionControl &control) const override {
		probe->authorization_open_calls.fetch_add(1, std::memory_order_relaxed);
		const auto alternative = AlternativeOf(authorization);
		// Query's credential provider supplies the kind-neutral CREDENTIAL
		// alternative for every authenticated relation (bearer or api_key).
		// BEARER remains valid for direct explicit construction, so either
		// non-anonymous alternative is authenticated.
		const bool authenticated = alternative != AuthorizationAlternative::ANONYMOUS;
		if (!authenticated) {
			probe->anonymous_authorizations.fetch_add(1, std::memory_order_relaxed);
		} else {
			probe->github_bearer_authorizations.fetch_add(1, std::memory_order_relaxed);
		}
		if (authenticated && plan.Operation().Protocol() == cuac::PlannedProtocol::REST &&
		    (plan.RelationName() != "authenticated_user" || plan.Authentication() != cuac::FeatureState::ENABLED ||
		     plan.Domain() != cuac::BaseDomain::SUCCESSFUL_ROOT_OBJECT ||
		     plan.Operation().Rest().cardinality != cuac::PlannedCardinality::EXACTLY_ONE_ON_SUCCESS)) {
			throw std::logic_error("bearer authorization received the wrong REST plan profile");
		}
		if (authenticated && plan.Operation().Protocol() == cuac::PlannedProtocol::GRAPHQL &&
		    (plan.RelationName() != "viewer_repository_metrics" ||
		     plan.Authentication() != cuac::FeatureState::ENABLED ||
		     plan.Domain() != cuac::BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES ||
		     plan.Operation().Graphql().cardinality != cuac::PlannedCardinality::ZERO_TO_MANY)) {
			throw std::logic_error("bearer authorization received the wrong GraphQL plan profile");
		}
		if (!authenticated && plan.Authentication() != cuac::FeatureState::DISABLED) {
			throw std::logic_error("anonymous authorization received an authenticated scan plan");
		}
		if (control.IsCancellationRequested()) {
			throw cuac::ExecutionCancelled();
		}
		if (scenario == QueryRuntimeScenario::OPEN_EXECUTION_CANCELLED) {
			throw cuac::ExecutionCancelled();
		}
		if (scenario == QueryRuntimeScenario::OPEN_INTERNAL_ERROR) {
			throw cuac::ExecutionError(cuac::ErrorStage::INTERNAL, "top-secret-open-field",
			                           "top-secret-open-structured-detail");
		}
		if (scenario == QueryRuntimeScenario::OPEN_POLICY_ERROR) {
			throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "authority", "request is outside the approved policy");
		}
		if (scenario == QueryRuntimeScenario::OPEN_RESOURCE_ERROR) {
			throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "response_bytes",
			                           "response exceeds its byte budget");
		}
		if (scenario == QueryRuntimeScenario::OPEN_LOCAL_ADMISSION_ERROR) {
			throw cuac::ExecutionError(
			    cuac::ErrorStage::RESOURCE, "admission", "local Runtime admission rejected scan",
			    cuac::LocalAdmissionFailureProperties(cuac::AdmissionReason::SCAN_QUEUE_SATURATED,
			                                          cuac::AdmissionScope::DESTINATION, 16, 16, 1, 0, false,
			                                          cuac::FailurePhase::ADMIT));
		}
		if (scenario == QueryRuntimeScenario::OPEN_UNKNOWN_EXCEPTION) {
			throw std::runtime_error("top-secret-open-unknown-detail");
		}
		if (scenario == QueryRuntimeScenario::NULL_STREAM) {
			return std::unique_ptr<cuac::BatchStream>();
		}
		const auto prior_streams = probe->streams_opened.fetch_add(1, std::memory_order_relaxed);
		auto stream_scenario = scenario;
		if ((scenario == QueryRuntimeScenario::LATE_RESOURCE_ERROR_ONCE ||
		     scenario == QueryRuntimeScenario::LATE_LOCAL_ADMISSION_ERROR_ONCE) &&
		    prior_streams > 0) {
			stream_scenario = QueryRuntimeScenario::SUCCESS;
		} else if (scenario == QueryRuntimeScenario::BLOCKING_ANONYMOUS_ONLY && authenticated) {
			stream_scenario = QueryRuntimeScenario::SUCCESS;
		}
		return std::unique_ptr<cuac::BatchStream>(
		    new QueryScenarioStream(stream_scenario, authenticated, plan.Operation().Protocol(), probe));
	}

private:
	QueryRuntimeScenario scenario;
	std::shared_ptr<QueryLifecycleProbe> probe;
};

} // namespace

QueryLifecycleProbe::QueryLifecycleProbe()
    : anonymous_open_calls(0), authorization_open_calls(0), anonymous_authorizations(0),
      github_bearer_authorizations(0), streams_opened(0), next_calls(0), batches(0), rows(0), cancellations(0),
      streams_closed(0), active_waiters(0), late_failure_enabled(false) {
}

std::shared_ptr<const cuac::ScanExecutor> BuildQueryScenarioExecutor(QueryRuntimeScenario scenario,
                                                                     std::shared_ptr<QueryLifecycleProbe> probe) {
	return std::shared_ptr<const cuac::ScanExecutor>(new QueryScenarioExecutor(scenario, std::move(probe)));
}

} // namespace cuac_test
