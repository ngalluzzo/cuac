#include "controlled_runtime_scenario.hpp"

#include "runtime/support/controlled_http_transport.hpp"

#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cuac_test {

struct ControlledRuntimeScenario::State {
	State(std::shared_ptr<ControlledHttpRuntime> runtime_p, uint64_t expected_request_count_p,
	      bool has_terminal_stage_p, cuac::ErrorStage terminal_stage_p)
	    : runtime(std::move(runtime_p)), expected_request_count(expected_request_count_p),
	      has_terminal_stage(has_terminal_stage_p), terminal_stage(terminal_stage_p), executor_close_count(0),
	      request_count(0), opened_stream_count(0), retained_stream_count(0), peak_retained_stream_count(0),
	      active_next_count(0), peak_active_next_count(0), completed_stream_count(0), cancelled_stream_count(0),
	      closed_stream_count(0), local_admission_rejection_count(0), slow_request_count(0),
	      ordinary_retry_failure_count(0), rate_limited_response_count(0), rate_limit_recovery_delay_milliseconds(0),
	      recovered_request_count(0), healthy_request_count(0), healthy_during_resilience_pressure_count(0),
	      unexpected_request_count(0) {
	}

	std::shared_ptr<ControlledHttpRuntime> runtime;
	uint64_t expected_request_count;
	bool has_terminal_stage;
	cuac::ErrorStage terminal_stage;
	std::atomic<uint64_t> executor_close_count;
	std::atomic<uint64_t> request_count;
	std::atomic<uint64_t> opened_stream_count;
	std::atomic<uint64_t> retained_stream_count;
	std::atomic<uint64_t> peak_retained_stream_count;
	std::atomic<uint64_t> active_next_count;
	std::atomic<uint64_t> peak_active_next_count;
	std::atomic<uint64_t> completed_stream_count;
	std::atomic<uint64_t> cancelled_stream_count;
	std::atomic<uint64_t> closed_stream_count;
	std::atomic<uint64_t> local_admission_rejection_count;
	std::atomic<uint64_t> slow_request_count;
	std::atomic<uint64_t> ordinary_retry_failure_count;
	std::atomic<uint64_t> rate_limited_response_count;
	std::atomic<uint64_t> rate_limit_recovery_delay_milliseconds;
	std::atomic<uint64_t> recovered_request_count;
	std::atomic<uint64_t> healthy_request_count;
	std::atomic<uint64_t> healthy_during_resilience_pressure_count;
	std::atomic<uint64_t> unexpected_request_count;
	mutable std::mutex request_mutex;
	std::condition_variable request_condition;
};

namespace {

std::string GraphqlNode(const char *language) {
	return std::string("{\"id\":\"R-duplicate\",\"nameWithOwner\":\"duckdb/duckdb\",") +
	       "\"owner\":{\"login\":\"duckdb\"},\"stargazerCount\":42,\"primaryLanguage\":" + language +
	       ",\"isPrivate\":false,\"isArchived\":false,\"updatedAt\":\"2026-07-01T00:00:00Z\"}";
}

std::string GraphqlRelationalNode(const char *id, int64_t stars, bool archived) {
	return std::string("{\"id\":\"") + id + "\",\"nameWithOwner\":\"fixture/" + id +
	       "\",\"owner\":{\"login\":\"fixture\"},\"stargazerCount\":" + std::to_string(stars) +
	       ",\"primaryLanguage\":null,\"isPrivate\":false,\"isArchived\":" + (archived ? "true" : "false") +
	       ",\"updatedAt\":\"2026-07-01T00:00:00Z\"}";
}

std::string GraphqlPage(const std::string &node, bool has_next, const char *cursor) {
	return "{\"data\":{\"viewer\":{\"repositories\":{\"nodes\":[" + node +
	       "],\"pageInfo\":{\"hasNextPage\":" + (has_next ? "true" : "false") + ",\"endCursor\":" + cursor +
	       "}}}},\"errors\":[]}";
}

std::string RickAndMortyHealthyResponse() {
	return "{\"info\":{\"count\":1,\"pages\":1,\"next\":null,\"prev\":null},\"results\":["
	       "{\"id\":1,\"name\":\"Rick Sanchez\",\"status\":\"Alive\",\"species\":\"Human\","
	       "\"origin\":{\"name\":\"Earth (C-137)\"},\"episode\":[]}]}";
}

void RecordPeak(std::atomic<uint64_t> &peak, uint64_t observed) noexcept {
	auto previous = peak.load(std::memory_order_relaxed);
	while (previous < observed &&
	       !peak.compare_exchange_weak(previous, observed, std::memory_order_relaxed, std::memory_order_relaxed)) {
	}
}

void RecordLocalAdmission(const std::shared_ptr<ControlledRuntimeScenario::State> &state,
                          const cuac::ExecutionError &error) noexcept {
	if (error.Classified() && error.Properties().failure_class == cuac::FailureClass::LOCAL_ADMISSION) {
		state->local_admission_rejection_count.fetch_add(1, std::memory_order_relaxed);
	}
}

class ObservedStream final : public cuac::BatchStream {
public:
	ObservedStream(std::shared_ptr<ControlledRuntimeScenario::State> state_p,
	               std::unique_ptr<cuac::BatchStream> inner_p)
	    : state(std::move(state_p)), inner(std::move(inner_p)), completed(false), cancelled(false), closed(false) {
		state->opened_stream_count.fetch_add(1, std::memory_order_relaxed);
		const auto retained = state->retained_stream_count.fetch_add(1, std::memory_order_relaxed) + 1;
		RecordPeak(state->peak_retained_stream_count, retained);
	}

	~ObservedStream() noexcept override {
		Close();
	}

	bool Next(cuac::ExecutionControl &control, cuac::TypedBatch &batch) override {
		const auto active = state->active_next_count.fetch_add(1, std::memory_order_relaxed) + 1;
		RecordPeak(state->peak_active_next_count, active);
		struct NextGuard {
			explicit NextGuard(std::atomic<uint64_t> &active_p) : active(active_p) {
			}
			~NextGuard() {
				active.fetch_sub(1, std::memory_order_relaxed);
			}
			std::atomic<uint64_t> &active;
		} guard(state->active_next_count);
		try {
			const bool has_batch = inner->Next(control, batch);
			if (!has_batch) {
				RecordCompleted();
			}
			return has_batch;
		} catch (const cuac::ExecutionCancelled &) {
			RecordCancelled();
			throw;
		} catch (const cuac::ExecutionError &error) {
			RecordLocalAdmission(state, error);
			throw;
		}
	}

	void Cancel() noexcept override {
		RecordCancelled();
		inner->Cancel();
	}

	void Close() noexcept override {
		bool expected = false;
		if (closed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			state->closed_stream_count.fetch_add(1, std::memory_order_relaxed);
			// This observes the public wrapper lifetime only. Runtime's internal
			// AdmissionController tests own proof that every permit dimension drains.
			state->retained_stream_count.fetch_sub(1, std::memory_order_relaxed);
			inner->Close();
		}
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		return inner->Diagnostics();
	}

private:
	void RecordCompleted() noexcept {
		bool expected = false;
		if (completed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
			state->completed_stream_count.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void RecordCancelled() noexcept {
		bool expected = false;
		if (cancelled.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
			state->cancelled_stream_count.fetch_add(1, std::memory_order_relaxed);
		}
	}

	const std::shared_ptr<ControlledRuntimeScenario::State> state;
	const std::unique_ptr<cuac::BatchStream> inner;
	std::atomic<bool> completed;
	std::atomic<bool> cancelled;
	std::atomic<bool> closed;
};

uint64_t RetainedFieldBytes(const std::vector<cuac::internal::HttpObservedHeader> &fields) {
	uint64_t result = static_cast<uint64_t>(fields.capacity()) * sizeof(cuac::internal::HttpObservedHeader);
	for (const auto &field : fields) {
		result += static_cast<uint64_t>(field.name.capacity()) + 1;
		result += static_cast<uint64_t>(field.value.capacity()) + 1;
	}
	return result;
}

class MixedResilienceTransport final : public cuac::internal::HttpTransport {
public:
	explicit MixedResilienceTransport(std::shared_ptr<ControlledRuntimeScenario::State> state_p)
	    : state(std::move(state_p)), api_request_count(0), has_rate_limit_time(false) {
	}

protected:
	cuac::internal::HttpResponse Get(const cuac::internal::HttpRequest &request,
	                                 const cuac::internal::HttpLimits &limits,
	                                 cuac::ExecutionControl &control) const override {
		return Respond(request, limits, control);
	}

	cuac::internal::HttpResponse Post(const cuac::internal::HttpRequest &request,
	                                  const cuac::internal::HttpLimits &limits,
	                                  cuac::ExecutionControl &control) const override {
		return Respond(request, limits, control);
	}

private:
	cuac::internal::HttpResponse Respond(const cuac::internal::HttpRequest &request,
	                                     const cuac::internal::HttpLimits &limits,
	                                     cuac::ExecutionControl &control) const {
		if (request.host == "rickandmortyapi.com") {
			state->healthy_request_count.fetch_add(1, std::memory_order_relaxed);
			if (state->recovered_request_count.load(std::memory_order_relaxed) == 0) {
				state->healthy_during_resilience_pressure_count.fetch_add(1, std::memory_order_relaxed);
			}
			RecordRequest();
			return Response(200, RickAndMortyHealthyResponse(), {}, limits);
		}
		if (request.host != "api.example.com" || request.target != "/duplicate-events") {
			state->unexpected_request_count.fetch_add(1, std::memory_order_relaxed);
			RecordRequest();
			throw cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "", "HTTP request failed");
		}

		const auto ordinal = api_request_count.fetch_add(1, std::memory_order_relaxed) + 1;
		if (ordinal == 1) {
			state->slow_request_count.fetch_add(1, std::memory_order_relaxed);
			RecordRequest();
			while (!control.IsCancellationRequested() && std::chrono::steady_clock::now() < limits.deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (control.IsCancellationRequested()) {
				throw cuac::ExecutionCancelled();
			}
			throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "wall_milliseconds",
			                           "execution exceeded its wall-time budget");
		}
		if (ordinal == 2) {
			state->ordinary_retry_failure_count.fetch_add(1, std::memory_order_relaxed);
			RecordRequest();
			throw cuac::internal::HttpAttemptFailure(
			    cuac::internal::HttpTransportFailureKind::RECEIVE_FAILED, {0, 0, 0, 0},
			    cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "", "HTTP request failed"));
		}
		if (ordinal == 3) {
			state->rate_limited_response_count.fetch_add(1, std::memory_order_relaxed);
			{
				std::lock_guard<std::mutex> guard(rate_limit_mutex);
				rate_limit_time = std::chrono::steady_clock::now();
				has_rate_limit_time = true;
			}
			RecordRequest();
			return Response(429, "",
			                {{"retry-after", "1"}, {"x-ratelimit-remaining", "0"}, {"x-ratelimit-resource", "core"}},
			                limits);
		}
		if (ordinal == 4) {
			{
				std::lock_guard<std::mutex> guard(rate_limit_mutex);
				if (!has_rate_limit_time) {
					state->unexpected_request_count.fetch_add(1, std::memory_order_relaxed);
					RecordRequest();
					throw cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "", "HTTP request failed");
				}
				const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
				                       std::chrono::steady_clock::now() - rate_limit_time)
				                       .count();
				state->rate_limit_recovery_delay_milliseconds.store(delay < 0 ? 0 : static_cast<uint64_t>(delay),
				                                                    std::memory_order_relaxed);
			}
			{
				std::unique_lock<std::mutex> guard(state->request_mutex);
				state->request_condition.wait_for(guard, std::chrono::seconds(2), [&]() {
					return control.IsCancellationRequested() ||
					       state->healthy_request_count.load(std::memory_order_relaxed) != 0;
				});
			}
			if (control.IsCancellationRequested()) {
				throw cuac::ExecutionCancelled();
			}
			state->recovered_request_count.fetch_add(1, std::memory_order_relaxed);
			RecordRequest();
			return Response(200,
			                "[{\"id\":\"duplicate\",\"ordinal\":1},{\"id\":\"duplicate\",\"ordinal\":1},"
			                "{\"id\":\"other\",\"ordinal\":2}]",
			                {}, limits);
		}
		state->unexpected_request_count.fetch_add(1, std::memory_order_relaxed);
		RecordRequest();
		throw cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "", "HTTP request failed");
	}

	void RecordRequest() const {
		{
			std::lock_guard<std::mutex> guard(state->request_mutex);
			state->request_count.fetch_add(1, std::memory_order_relaxed);
		}
		state->request_condition.notify_all();
	}

	static cuac::internal::HttpResponse Response(uint32_t status, std::string body,
	                                             std::vector<cuac::internal::HttpObservedHeader> fields,
	                                             const cuac::internal::HttpLimits &limits) {
		std::vector<cuac::internal::HttpObservedHeader> retained;
		for (auto &field : fields) {
			if (std::find(limits.retained_header_names.begin(), limits.retained_header_names.end(), field.name) !=
			    limits.retained_header_names.end()) {
				retained.push_back(std::move(field));
			}
		}
		const auto metadata_bytes = RetainedFieldBytes(retained);
		if (static_cast<uint64_t>(body.size()) > limits.max_response_bytes ||
		    static_cast<uint64_t>(body.size()) > limits.max_decompressed_bytes ||
		    metadata_bytes > limits.max_metadata_bytes) {
			throw cuac::ExecutionError(cuac::ErrorStage::RESOURCE, "response_bytes",
			                           "HTTP response exceeded its byte budget");
		}
		const auto body_bytes = static_cast<uint64_t>(body.size());
		return {status,          64,
		        body_bytes,      body_bytes,
		        std::move(body), {{}, metadata_bytes, status == 429, std::move(retained), {}}};
	}

	const std::shared_ptr<ControlledRuntimeScenario::State> state;
	mutable std::atomic<uint64_t> api_request_count;
	mutable std::mutex rate_limit_mutex;
	mutable std::chrono::steady_clock::time_point rate_limit_time;
	mutable bool has_rate_limit_time;
};

// Public-interface observer used by Query's lifecycle tests. It forwards every
// open through ScanExecutor's bounded public envelopes and records content-free
// stream and idempotent-close transitions; no Runtime-private controller crosses
// the team service boundary.
class ObservedExecutor final : public cuac::ScanExecutor {
public:
	ObservedExecutor(std::shared_ptr<ControlledRuntimeScenario::State> state_p,
	                 std::shared_ptr<const cuac::ScanExecutor> inner_p)
	    : state(std::move(state_p)), inner(std::move(inner_p)), closed(false) {
	}

	std::unique_ptr<cuac::BatchStream> Open(const cuac::ScanPlan &plan,
	                                        cuac::ExecutionControl &control) const override {
		return OpenObserved([&]() { return inner->Open(plan, control); });
	}

	void Close() const noexcept override {
		bool expected = false;
		if (closed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			state->executor_close_count.fetch_add(1, std::memory_order_relaxed);
			inner->Close();
		}
	}

protected:
	std::unique_ptr<cuac::BatchStream> OpenAuthorizationEnvelope(const cuac::ScanPlan &plan,
	                                                             cuac::ScanAuthorization authorization,
	                                                             cuac::ExecutionControl &control) const override {
		return OpenObserved([&]() { return inner->OpenWithAuthorization(plan, std::move(authorization), control); });
	}

	std::unique_ptr<cuac::BatchStream> OpenCredentialProviderEnvelope(const cuac::ScanPlan &plan,
	                                                                  const cuac::CredentialProvider &provider,
	                                                                  cuac::ExecutionControl &control) const override {
		return OpenObserved([&]() { return inner->OpenWithCredentialProvider(plan, provider, control); });
	}

private:
	template <class OpenFunction>
	std::unique_ptr<cuac::BatchStream> OpenObserved(OpenFunction open) const {
		try {
			auto stream = open();
			return std::unique_ptr<cuac::BatchStream>(new ObservedStream(state, std::move(stream)));
		} catch (const cuac::ExecutionError &error) {
			RecordLocalAdmission(state, error);
			throw;
		}
	}

	const std::shared_ptr<ControlledRuntimeScenario::State> state;
	const std::shared_ptr<const cuac::ScanExecutor> inner;
	mutable std::atomic<bool> closed;
};

} // namespace

ControlledRuntimeScenario::ControlledRuntimeScenario(std::shared_ptr<State> state_p,
                                                     std::shared_ptr<const cuac::ScanExecutor> executor_p)
    : state(std::move(state_p)), executor(std::move(executor_p)) {
}

std::shared_ptr<const cuac::ScanExecutor> ControlledRuntimeScenario::Executor() const {
	return executor;
}

ControlledRuntimeScenarioObservation ControlledRuntimeScenario::Observation() const {
	const auto requests = state->runtime ? static_cast<uint64_t>(state->runtime->Observations().size())
	                                     : state->request_count.load(std::memory_order_relaxed);
	return {requests,
	        state->expected_request_count,
	        state->has_terminal_stage,
	        state->terminal_stage,
	        state->executor_close_count.load(std::memory_order_relaxed),
	        state->opened_stream_count.load(std::memory_order_relaxed),
	        state->retained_stream_count.load(std::memory_order_relaxed),
	        state->peak_retained_stream_count.load(std::memory_order_relaxed),
	        state->active_next_count.load(std::memory_order_relaxed),
	        state->peak_active_next_count.load(std::memory_order_relaxed),
	        state->completed_stream_count.load(std::memory_order_relaxed),
	        state->cancelled_stream_count.load(std::memory_order_relaxed),
	        state->closed_stream_count.load(std::memory_order_relaxed),
	        state->local_admission_rejection_count.load(std::memory_order_relaxed),
	        state->slow_request_count.load(std::memory_order_relaxed),
	        state->ordinary_retry_failure_count.load(std::memory_order_relaxed),
	        state->rate_limited_response_count.load(std::memory_order_relaxed),
	        state->rate_limit_recovery_delay_milliseconds.load(std::memory_order_relaxed),
	        state->recovered_request_count.load(std::memory_order_relaxed),
	        state->healthy_request_count.load(std::memory_order_relaxed),
	        state->healthy_during_resilience_pressure_count.load(std::memory_order_relaxed),
	        state->unexpected_request_count.load(std::memory_order_relaxed)};
}

bool ControlledRuntimeScenario::WaitForRequestCount(uint64_t count, uint64_t timeout_milliseconds) const {
	if (state->runtime) {
		return state->runtime->WaitForRequestCount(count, std::chrono::milliseconds(timeout_milliseconds));
	}
	std::unique_lock<std::mutex> guard(state->request_mutex);
	return state->request_condition.wait_for(guard, std::chrono::milliseconds(timeout_milliseconds), [&]() {
		return state->request_count.load(std::memory_order_relaxed) >= count;
	});
}

std::shared_ptr<ControlledRuntimeScenario> BuildControlledRuntimeScenario(ControlledRuntimeScenarioId scenario) {
	if (scenario == ControlledRuntimeScenarioId::MIXED_RESILIENCE_PRESSURE) {
		auto state = std::make_shared<ControlledRuntimeScenario::State>(nullptr, 5, false, cuac::ErrorStage::INTERNAL);
		auto runtime = cuac::internal::BuildHttpScanExecutor(
		    std::unique_ptr<cuac::internal::HttpTransport>(new MixedResilienceTransport(state)));
		auto observed = std::make_shared<ObservedExecutor>(state, std::move(runtime));
		return std::shared_ptr<ControlledRuntimeScenario>(
		    new ControlledRuntimeScenario(std::move(state), std::move(observed)));
	}
	std::shared_ptr<ControlledHttpRuntime> runtime;
	if (scenario == ControlledRuntimeScenarioId::RICKANDMORTY_CHARACTER_EPISODES) {
		runtime = BuildControlledHttpRuntimeForHost("rickandmortyapi.com");
	} else if (scenario == ControlledRuntimeScenarioId::REST_RETRY_TRANSIENT_DUPLICATE ||
	           scenario == ControlledRuntimeScenarioId::ADMISSION_REST_SATURATION ||
	           scenario == ControlledRuntimeScenarioId::ADMISSION_GRAPHQL_SATURATION) {
		runtime = BuildControlledPackageHttpRuntime();
	} else {
		runtime = BuildControlledHttpRuntime();
	}
	uint64_t expected_request_count = 1;
	bool has_terminal_stage = false;
	auto terminal_stage = cuac::ErrorStage::INTERNAL;
	switch (scenario) {
	case ControlledRuntimeScenarioId::RETAINED_REST_USER:
		runtime->Respond(200, "{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false}");
		break;
	case ControlledRuntimeScenarioId::RICKANDMORTY_CHARACTER_EPISODES:
		expected_request_count = 2;
		{
			const auto response = ControlledResponse(
			    200, "{\"info\":{\"count\":4,\"pages\":1,\"next\":null,\"prev\":null},\"results\":["
			         "{\"id\":4,\"name\":\"Beth Smith\",\"status\":\"Alive\",\"species\":\"Human\","
			         "\"origin\":{\"name\":\"Earth (Replacement Dimension)\"},\"episode\":["
			         "\"https://rickandmortyapi.com/api/episode/4\","
			         "\"https://rickandmortyapi.com/api/episode/1\","
			         "\"https://rickandmortyapi.com/api/episode/4\"]},"
			         "{\"id\":1,\"name\":\"Rick Sanchez\",\"status\":\"Alive\",\"species\":\"Human\","
			         "\"origin\":{\"name\":\"Earth (C-137)\"},\"episode\":["
			         "\"https://rickandmortyapi.com/api/episode/1\","
			         "\"https://rickandmortyapi.com/api/episode/2\"]},"
			         "{\"id\":3,\"name\":\"Summer Smith\",\"status\":\"Alive\",\"species\":\"Human\","
			         "\"origin\":{\"name\":\"Earth (Replacement Dimension)\"},\"episode\":[]},"
			         "{\"id\":2,\"name\":\"Morty Smith\",\"status\":\"Alive\",\"species\":\"Human\","
			         "\"origin\":{\"name\":\"unknown\"},\"episode\":["
			         "\"https://rickandmortyapi.com/api/episode/2\"]}]}");
			runtime->RespondSequence({response, response});
		}
		break;
	case ControlledRuntimeScenarioId::GRAPHQL_MULTI_PAGE_NULL_DUPLICATE: {
		expected_request_count = 4;
		const auto first_page =
		    ControlledResponse(200, GraphqlPage(GraphqlNode("null"), true, "\"runtime-owned-next\""));
		const auto second_page = ControlledResponse(200, GraphqlPage(GraphqlNode("{\"name\":\"C++\"}"), false, "null"));
		runtime->RespondSequence({first_page, second_page, first_page, second_page});
		break;
	}
	case ControlledRuntimeScenarioId::GRAPHQL_RELATIONAL_COMPOSITION:
		runtime->Respond(200, GraphqlPage(GraphqlRelationalNode("R-archived-high", 999, true) + "," +
		                                      GraphqlRelationalNode("R-active-low", 7, false) + "," +
		                                      GraphqlRelationalNode("R-active-high", 42, false),
		                                  false, "null"));
		break;
	case ControlledRuntimeScenarioId::GRAPHQL_APPLICATION_ERROR:
		has_terminal_stage = true;
		terminal_stage = cuac::ErrorStage::REMOTE_PROTOCOL;
		runtime->Respond(200, "{\"data\":null,\"errors\":[{\"message\":\"runtime-owned private canary\"}]}");
		break;
	case ControlledRuntimeScenarioId::GRAPHQL_LATE_HTTP_STATUS:
		expected_request_count = 2;
		has_terminal_stage = true;
		terminal_stage = cuac::ErrorStage::HTTP_STATUS;
		runtime->RespondSequence(
		    {ControlledResponse(200, GraphqlPage(GraphqlNode("null"), true, "\"runtime-owned-late\"")),
		     ControlledResponse(429, "runtime-owned private status body")});
		break;
	case ControlledRuntimeScenarioId::REST_RETRY_TRANSIENT_DUPLICATE: {
		expected_request_count = 6;
		const auto transient =
		    ControlledTransientTransportFailure(cuac::internal::HttpTransportFailureKind::RECEIVE_FAILED);
		const auto page =
		    ControlledResponse(200, "[{\"id\":\"duplicate\",\"ordinal\":1},{\"id\":\"duplicate\",\"ordinal\":1},"
		                            "{\"id\":\"other\",\"ordinal\":2}]");
		runtime->RespondSequence(
		    {ControlledResponse(503, ""), transient, page, ControlledResponse(503, ""), transient, page});
		break;
	}
	case ControlledRuntimeScenarioId::ADMISSION_REST_SATURATION:
	case ControlledRuntimeScenarioId::ADMISSION_GRAPHQL_SATURATION:
		expected_request_count = 2;
		runtime->BlockHostAndRespondElsewhere("api.github.com", 200, RickAndMortyHealthyResponse());
		break;
	case ControlledRuntimeScenarioId::BLOCK_UNTIL_CANCEL:
		runtime->BlockUntilCancelled();
		break;
	case ControlledRuntimeScenarioId::MIXED_RESILIENCE_PRESSURE:
		throw std::logic_error("mixed resilience scenario was not constructed through its dedicated transport");
	default:
		throw std::invalid_argument("unknown controlled Runtime scenario");
	}
	auto state = std::make_shared<ControlledRuntimeScenario::State>(runtime, expected_request_count, has_terminal_stage,
	                                                                terminal_stage);
	auto observed = std::make_shared<ObservedExecutor>(state, runtime->Executor());
	return std::shared_ptr<ControlledRuntimeScenario>(
	    new ControlledRuntimeScenario(std::move(state), std::move(observed)));
}

} // namespace cuac_test
