#include "package_fixture_execution.hpp"

#include "runtime/support/controlled_http_transport.hpp"
#include "runtime/support/package_fixture_observation_internal.hpp"
#include "runtime/support/package_fixture_scenario_internal.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cuac_test {
namespace {

static const char FIXTURE_BEARER_TOKEN[] = "runtime_fixture_bearer_capability";

std::unique_ptr<cuac::BatchStream> OpenFixtureStream(const cuac::ScanPlan &plan,
                                                     const RuntimeFixtureTranscript &transcript,
                                                     ControlledHttpRuntime &runtime, cuac::ExecutionControl &control) {
	switch (transcript.authorization) {
	case RuntimeFixtureAuthorizationState::ANONYMOUS:
		return runtime.Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Anonymous(), control);
	case RuntimeFixtureAuthorizationState::BEARER_PRESENT:
		runtime.ExpectBearer(std::string("Bearer ") + FIXTURE_BEARER_TOKEN);
		return runtime.Executor()->OpenWithAuthorization(
		    plan, cuac::ScanAuthorization::Bearer(std::string(FIXTURE_BEARER_TOKEN)), control);
	case RuntimeFixtureAuthorizationState::BEARER_MISSING: {
		auto stream = runtime.Executor()->Open(plan, control);
		if (stream) {
			stream->Close();
		}
		throw std::logic_error("bearer-missing fixture did not fail before stream admission");
	}
	default:
		throw std::invalid_argument("unknown controlled fixture authorization state");
	}
}

void CancelAndClose(std::unique_ptr<cuac::BatchStream> &stream, RuntimeFixtureExecutionObservation &result) noexcept {
	if (!stream) {
		return;
	}
	stream->Cancel();
	result.stream_cancel_invoked = true;
	stream->Close();
	result.stream_close_invoked = true;
}

void Close(std::unique_ptr<cuac::BatchStream> &stream, RuntimeFixtureExecutionObservation &result) noexcept {
	if (!stream) {
		return;
	}
	stream->Close();
	result.stream_close_invoked = true;
}

RuntimeFixtureExecutionObservation
RunFixtureScenarioImpl(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript,
                       RuntimeFixtureScenario scenario, cuac::ExecutionControl &outer_control,
                       bool capture_cancellation,
                       const internal::RuntimeFixtureResponseAccountingOverrides *overrides) {
	internal::ValidateRuntimeFixtureScenario(scenario);
	auto result = internal::NewRuntimeFixtureObservation(plan, scenario.Cancellation());
	internal::RuntimeFixtureScenarioControl control(outer_control, scenario.Cancellation());
	auto runtime = BuildControlledPackageHttpRuntime();
	std::unique_ptr<cuac::BatchStream> stream;
	std::thread transport_canceller;
	std::exception_ptr cancellation;

	try {
		if (control.IsCancellationRequested()) {
			throw cuac::ExecutionCancelled();
		}
		internal::ValidateRuntimeFixtureTranscript(transcript);
		if (transcript.authorization != RuntimeFixtureAuthorizationState::BEARER_MISSING) {
			const bool transport_failure = scenario.Failure() == RuntimeFixtureFailureExpectation::TRANSPORT;
			runtime->RespondSequence(
			    internal::BuildRuntimeFixtureResponses(transcript, control, transport_failure, overrides));
		}
		stream = OpenFixtureStream(plan, transcript, *runtime, control);

		if (scenario.Cancellation() == RuntimeFixtureCancellationPoint::BEFORE_REQUEST) {
			control.Reach(RuntimeFixtureCancellationPoint::BEFORE_REQUEST);
		}
		if (scenario.Cancellation() == RuntimeFixtureCancellationPoint::TRANSPORT) {
			runtime->BlockUntilCancelled();
			transport_canceller = std::thread([&]() {
				if (runtime->WaitForRequestCount(1, std::chrono::seconds(2))) {
					control.Reach(RuntimeFixtureCancellationPoint::TRANSPORT);
				}
			});
		}

		while (true) {
			cuac::TypedBatch batch;
			bool produced = false;
			{
				RuntimeFixtureCheckpointScope checkpoint_scope(control);
				produced = stream->Next(control, batch);
			}
			if (!produced) {
				break;
			}
			for (auto &row : batch.rows) {
				result.rows.push_back(std::move(row));
			}

			if (scenario.Cancellation() == RuntimeFixtureCancellationPoint::PAGE_BOUNDARY) {
				// A successful Next means transport and the complete production page
				// decode committed before any row became observable. That is the same
				// decoded-page checkpoint for a disabled one-page operation and for a
				// paginated first page; cancellation discards the moved rows below.
				control.Reach(RuntimeFixtureCancellationPoint::PAGE_BOUNDARY);
			}

			if (scenario.Cancellation() == RuntimeFixtureCancellationPoint::STREAM_CLOSE) {
				control.Reach(RuntimeFixtureCancellationPoint::STREAM_CLOSE);
				CancelAndClose(stream, result);
				cuac::TypedBatch after_close;
				result.post_close_exhaustion_observed = !stream->Next(control, after_close) && after_close.rows.empty();
				result.rows.clear();
				result.cancellation_observed = true;
				result.checkpoint_reached = control.CheckpointReached();
				break;
			}
		}

		if (!result.cancellation_observed) {
			Close(stream, result);
			result.succeeded = true;
		}
	} catch (const cuac::ExecutionCancelled &) {
		CancelAndClose(stream, result);
		result.rows.clear();
		result.cancellation_observed = true;
		result.checkpoint_reached = control.CheckpointReached();
		cancellation = std::current_exception();
	} catch (const cuac::ExecutionError &error) {
		Close(stream, result);
		result.rows.clear();
		result.has_runtime_error = true;
		result.runtime_error_stage = error.Stage();
		result.runtime_error_field = error.Field();
	} catch (...) {
		CancelAndClose(stream, result);
		if (transport_canceller.joinable()) {
			transport_canceller.join();
		}
		throw;
	}

	if (transport_canceller.joinable()) {
		transport_canceller.join();
	}
	if (scenario.Cancellation() != RuntimeFixtureCancellationPoint::NONE && !control.CheckpointReached()) {
		throw std::logic_error("fixture scenario did not reach its selected Runtime cancellation checkpoint");
	}
	internal::CaptureRuntimeFixtureRequests(transcript, *runtime, result);
	internal::ValidateRuntimeFixtureFailure(scenario.Failure(), result);
	if (cancellation && !capture_cancellation) {
		std::rethrow_exception(cancellation);
	}
	return result;
}

} // namespace

namespace internal {

RuntimeFixtureExecutionObservation
RunRuntimeFixtureScenario(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript,
                          RuntimeFixtureScenario scenario, cuac::ExecutionControl &outer_control,
                          bool capture_cancellation, const RuntimeFixtureResponseAccountingOverrides *overrides) {
	return RunFixtureScenarioImpl(plan, transcript, scenario, outer_control, capture_cancellation, overrides);
}

} // namespace internal

RuntimeFixtureExecutionObservation
RuntimePackageFixtureExecutionService::Execute(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript,
                                               cuac::ExecutionControl &control) const {
	return internal::RunRuntimeFixtureScenario(plan, transcript, RuntimeFixtureScenario::Standard(), control, false);
}

RuntimeFixtureExecutionObservation RuntimePackageFixtureExecutionService::ExecuteScenario(
    const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript, RuntimeFixtureScenario scenario,
    cuac::ExecutionControl &control) const {
	return internal::RunRuntimeFixtureScenario(plan, transcript, scenario, control, true);
}

} // namespace cuac_test
