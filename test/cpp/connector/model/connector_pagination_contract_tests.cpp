#include "cuac/connector/catalog.hpp"
#include "connector/support/catalog_test_access.hpp"
#include "connector/support/pagination_contract.hpp"
#include "support/require.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cuac_test::ConnectorCatalogTestAccess;
using cuac_test::Require;

#define DEFINE_MEMBER_PROBE(PROBE_NAME, MEMBER_NAME)                                                                   \
	template <typename T>                                                                                              \
	class PROBE_NAME {                                                                                                 \
		template <typename U>                                                                                          \
		static char Test(decltype(&U::MEMBER_NAME));                                                                   \
		template <typename U>                                                                                          \
		static long Test(...);                                                                                         \
                                                                                                                       \
	public:                                                                                                            \
		static const bool VALUE = sizeof(Test<T>(0)) == sizeof(char);                                                  \
	}

DEFINE_MEMBER_PROBE(HasPaginationEnabledMember, pagination_enabled);
DEFINE_MEMBER_PROBE(HasResponseUrlMember, response_url);
DEFINE_MEMBER_PROBE(HasLinkValueMember, link_value);

#undef DEFINE_MEMBER_PROBE

static_assert(!HasPaginationEnabledMember<cuac::CompiledOperation>::VALUE,
              "pagination must be an explicit closed declaration rather than a boolean");
static_assert(!HasResponseUrlMember<cuac::CompiledPagination>::VALUE,
              "compiled pagination must not retain a response-granted URL");
static_assert(!HasLinkValueMember<cuac::CompiledPagination>::VALUE,
              "compiled pagination must not retain mutable Link response data");
static_assert(std::is_same<decltype(cuac::CompiledRestOperation::pagination), cuac::CompiledPagination>::value,
              "the REST protocol alternative must carry the closed pagination value");
static_assert(std::is_copy_constructible<cuac::CompiledPagination>::value,
              "immutable pagination must support relation/catalog copies");
static_assert(std::is_move_constructible<cuac::CompiledPagination>::value,
              "immutable pagination must support relation/catalog ownership transfer");
static_assert(!std::is_copy_assignable<cuac::CompiledPagination>::value,
              "pagination assignment would permit post-construction replacement");
static_assert(!std::is_move_assignable<cuac::CompiledPagination>::value,
              "pagination assignment would permit post-construction replacement");
static_assert(!std::is_default_constructible<cuac::CompiledPagination>::value,
              "pagination must not admit a payload-free ambiguous default");
static_assert(!std::is_constructible<cuac::CompiledPagination, bool>::value,
              "production consumers must not enable pagination with a boolean");
static_assert(std::is_copy_constructible<cuac::CompiledResourceCeilings>::value,
              "immutable resource declarations must support catalog copies");
static_assert(!std::is_default_constructible<cuac::CompiledResourceCeilings>::value,
              "resource declarations must not admit a partial default");
static_assert(!std::is_constructible<cuac::CompiledResourceCeilings, std::uint64_t, std::uint64_t>::value,
              "production consumers must not construct relation resource authority");

template <typename Callable>
void RequireInvalid(const std::string &message, Callable callback) {
	bool rejected = false;
	try {
		callback();
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, message);
}

template <typename Callable>
void RequireLogicError(const std::string &message, Callable callback) {
	bool rejected = false;
	try {
		callback();
	} catch (const std::logic_error &) {
		rejected = true;
	}
	Require(rejected, message);
}

std::vector<cuac::CompiledColumn> Columns() {
	return {{"id", "BIGINT", false, "$.id"}};
}

cuac::CompiledOperation BuildDisabledOperation() {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("api.github.com"),
	                                         443};
	return cuac::CompiledOperation {"fixture_rows",
	                                true,
	                                cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                                cuac::CompiledProtocol::REST,
	                                cuac::CompiledHttpMethod::GET,
	                                cuac::CompiledReplaySafety::SAFE,
	                                false,
	                                ConnectorCatalogTestAccess::DisabledPagination(),
	                                {origin, "/rows", {}, {{"X-Fixture", "safe"}}},
	                                cuac::CompiledResponseSource::JSON_PATH_MANY,
	                                "$.items[*]",
	                                cuac::CompiledOperationSelector()};
}

cuac::CompiledOperation BuildPaginatedOperation(cuac::CompiledPagination pagination,
                                                std::vector<cuac::CompiledQueryParameter> query_parameters = {
                                                    ConnectorCatalogTestAccess::PageSizeQuery("page_size", 5),
                                                    ConnectorCatalogTestAccess::PageNumberQuery("page", 1)}) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("api.github.com"),
	                                         443};
	return cuac::CompiledOperation {"fixture_link_rows",
	                                true,
	                                cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                                cuac::CompiledProtocol::REST,
	                                cuac::CompiledHttpMethod::GET,
	                                cuac::CompiledReplaySafety::SAFE,
	                                false,
	                                std::move(pagination),
	                                {origin, "/linked-rows", std::move(query_parameters), {{"X-Fixture", "safe"}}},
	                                cuac::CompiledResponseSource::JSON_PATH_MANY,
	                                "$.items[*]",
	                                cuac::CompiledOperationSelector()};
}

cuac::CompiledConnector BuildValidPaginatedCatalog() {
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    "linked_rows", Columns(),
	    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4)),
	    ConnectorCatalogTestAccess::RequiredBearer(),
	    ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 20, 64)));
	return ConnectorCatalogTestAccess::Catalog(
	    "pagination_fixture", "1.0.0", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"api.github.com"}, false, false, false, false, 2048});
}

void TestClosedValuesAndExplanation() {
	const auto unpaginated = ConnectorCatalogTestAccess::Relation(
	    "rows", Columns(), BuildDisabledOperation(), ConnectorCatalogTestAccess::Anonymous(),
	    ConnectorCatalogTestAccess::UnpaginatedResources(2, 64));
	Require(unpaginated.Operation().Rest().pagination.Strategy() == cuac::CompiledPaginationStrategy::DISABLED,
	        "unpaginated relation gained a pagination declaration");
	Require(!unpaginated.ResourceCeilings().HasResponseByteNarrowing() &&
	            unpaginated.ResourceCeilings().MaxRecordsPerPage() ==
	                unpaginated.ResourceCeilings().MaxRecordsPerScan(),
	        "unpaginated relation lost its one-page resource scope");
	RequireLogicError("disabled pagination exposed Link payload",
	                  [&unpaginated]() { (void)unpaginated.Operation().Rest().pagination.PageSizeParameter(); });
	RequireLogicError("unscoped resources exposed response-byte payload",
	                  [&unpaginated]() { (void)unpaginated.ResourceCeilings().MaxResponseBytesPerPage(); });

	const auto catalog = BuildValidPaginatedCatalog();
	const auto &paginated = catalog.Relations()[0];
	const auto &pagination = paginated.Operation().Rest().pagination;
	Require(pagination.Strategy() == cuac::CompiledPaginationStrategy::LINK_HEADER &&
	            pagination.Dependency() == cuac::CompiledPageDependency::SEQUENTIAL &&
	            pagination.Consistency() == cuac::CompiledPageConsistency::MUTABLE &&
	            pagination.LinkRelation() == cuac::CompiledLinkRelation::NEXT,
	        "paginated relation lost its closed capability declaration");
	Require(pagination.TargetScope() == cuac::CompiledContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH &&
	            !pagination.SupportsTotal() && !pagination.SupportsResume(),
	        "paginated relation widened continuation or consistency claims");
	Require(paginated.ResourceCeilings().HasResponseByteNarrowing() &&
	            paginated.ResourceCeilings().MaxResponseBytesPerPage() == 1024 &&
	            paginated.ResourceCeilings().MaxResponseBytesPerScan() == 4096 &&
	            paginated.ResourceCeilings().MaxRecordsPerPage() == 5 &&
	            paginated.ResourceCeilings().MaxRecordsPerScan() == 20,
	        "paginated relation lost distinct page and scan resources");
	Require(catalog.Snapshot().find("pagination:link_header[relation:next,dependency:sequential,") != std::string::npos,
	        "safe snapshot omitted explicit pagination semantics");
	Require(catalog.Snapshot().find(
	            "response_bytes_per_page:1024,response_bytes_per_scan:4096,records_per_page:5,records_per_scan:20") !=
	            std::string::npos,
	        "safe snapshot omitted scoped pagination resources");
	for (const auto &prohibited : {"response_url=", "Link=", "secret_name=", "Authorization="}) {
		Require(catalog.Snapshot().find(prohibited) == std::string::npos,
		        "pagination explanation retained execution authority: " + std::string(prohibited));
	}
}

void TestPaginationProfileValidation() {
	RequireInvalid("Link pagination accepted an empty page-size parameter",
	               []() { (void)ConnectorCatalogTestAccess::SequentialLink("", 5, "page", 1, 1, 4); });
	for (const auto &name : {std::string("bad name"), std::string("bad%name"), std::string("bad\0name", 8),
	                         std::string("bad\tname"), std::string(u8"página"), std::string(64, 'a')}) {
		RequireInvalid("Link pagination accepted a page-size name outside the shared query grammar",
		               [&name]() { (void)ConnectorCatalogTestAccess::SequentialLink(name, 5, "page", 1, 1, 4); });
		RequireInvalid("Link pagination accepted a page-number name outside the shared query grammar",
		               [&name]() { (void)ConnectorCatalogTestAccess::SequentialLink("page_size", 5, name, 1, 1, 4); });
	}
	RequireInvalid("Link pagination accepted duplicate page parameter names",
	               []() { (void)ConnectorCatalogTestAccess::SequentialLink("page", 5, "page", 1, 1, 4); });
	RequireInvalid("Link pagination accepted a zero page size",
	               []() { (void)ConnectorCatalogTestAccess::SequentialLink("page_size", 0, "page", 1, 1, 4); });
	RequireInvalid("Link pagination accepted a zero first page",
	               []() { (void)ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 0, 1, 4); });
	RequireInvalid("Link pagination accepted a zero page transition",
	               []() { (void)ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 0, 4); });
	RequireInvalid("Link pagination accepted an empty scan page budget",
	               []() { (void)ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 0); });
	const auto bigint_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	RequireInvalid("Link pagination accepted a page sequence beyond BIGINT request authority", [=]() {
		(void)ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", bigint_max, 1, 2);
	});
	{
		auto operation = BuildPaginatedOperation(
		    ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", bigint_max - 2, 2, 2),
		    {ConnectorCatalogTestAccess::PageSizeQuery("page_size", 5),
		     ConnectorCatalogTestAccess::PageNumberQuery("page", bigint_max - 2)});
		const auto relation = ConnectorCatalogTestAccess::Relation(
		    "bigint_boundary_pages", Columns(), std::move(operation), ConnectorCatalogTestAccess::RequiredBearer(),
		    ConnectorCatalogTestAccess::PaginatedResources(1024, 2048, 5, 10, 64));
		Require(relation.Operation().Rest().pagination.FirstPage() == bigint_max - 2 &&
		            relation.Operation().Rest().pagination.PageIncrement() == 2 &&
		            relation.Snapshot().find("increment:2") != std::string::npos,
		        "Link pagination rejected or hid its last representable increment-two sequence");
	}
	RequireInvalid("Link pagination accepted an increment-two sequence one past BIGINT request authority", [=]() {
		(void)ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", bigint_max - 1, 2, 2);
	});

	RequireInvalid("Link pagination inferred a mismatched fixed page size", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4),
		                            {ConnectorCatalogTestAccess::PageSizeQuery("page_size", 4),
		                             ConnectorCatalogTestAccess::PageNumberQuery("page", 1)});
		ConnectorCatalogTestAccess::Relation("mismatched_page_size", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 20, 64));
	});
	{
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4),
		                            {ConnectorCatalogTestAccess::PageNumberQuery("page", 1),
		                             ConnectorCatalogTestAccess::FixedQuery("filter", "a b"),
		                             ConnectorCatalogTestAccess::PageSizeQuery("page_size", 5)});
		const auto relation = ConnectorCatalogTestAccess::Relation(
		    "reordered_page_bindings", Columns(), std::move(operation), ConnectorCatalogTestAccess::RequiredBearer(),
		    ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 20, 64));
		Require(relation.Operation().Rest().request.query_parameters.size() == 3 &&
		            relation.Operation().Rest().request.query_parameters[0].source ==
		                cuac::CompiledQueryValueSource::PAGE_NUMBER &&
		            relation.Operation().Rest().request.query_parameters[2].source ==
		                cuac::CompiledQueryValueSource::PAGE_SIZE,
		        "Link pagination recovered page roles from order or parameter spelling");
	}
	RequireInvalid("Link pagination accepted swapped structural page roles", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4),
		                            {ConnectorCatalogTestAccess::PageNumberQuery("page_size", 5),
		                             ConnectorCatalogTestAccess::PageSizeQuery("page", 1)});
		ConnectorCatalogTestAccess::Relation("swapped_page_roles", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 20, 64));
	});
	RequireInvalid("Link pagination accepted duplicate structural page-size roles", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4),
		                            {ConnectorCatalogTestAccess::PageSizeQuery("page_size", 5),
		                             ConnectorCatalogTestAccess::PageSizeQuery("page", 1)});
		ConnectorCatalogTestAccess::Relation("duplicate_page_roles", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 20, 64));
	});
	RequireInvalid("root-object operation accepted Link pagination", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4));
		operation.cardinality = cuac::CompiledOperationCardinality::EXACTLY_ONE_ON_SUCCESS;
		auto rest = operation.Rest();
		rest.response_source = cuac::CompiledResponseSource::ROOT_OBJECT;
		rest.records_extractor = "$";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		ConnectorCatalogTestAccess::Relation("paginated_root", Columns(), std::move(changed),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 1, 4, 64));
	});
	{
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4));
		auto rest = operation.Rest();
		rest.response_source = cuac::CompiledResponseSource::ROOT_ARRAY;
		rest.records_extractor = "$";
		auto changed = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest), operation.selector);
		const auto root_array = ConnectorCatalogTestAccess::Relation(
		    "paginated_root_array", Columns(), std::move(changed), ConnectorCatalogTestAccess::RequiredBearer(),
		    ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 20, 64));
		Require(root_array.Operation().Rest().response_source == cuac::CompiledResponseSource::ROOT_ARRAY,
		        "typed paginated root-array source was not retained");
	}
}

void TestScopedResourceValidation() {
	RequireInvalid("relation accepted an empty resource ceiling", []() {
		ConnectorCatalogTestAccess::Relation("empty_resources", Columns(), BuildDisabledOperation(),
		                                     ConnectorCatalogTestAccess::Anonymous(),
		                                     ConnectorCatalogTestAccess::UnpaginatedResources(2, 0));
	});
	RequireInvalid("unpaginated relation accepted different page and scan records", []() {
		ConnectorCatalogTestAccess::Relation("widened_unpaginated", Columns(), BuildDisabledOperation(),
		                                     ConnectorCatalogTestAccess::Anonymous(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 1024, 2, 4, 64));
	});
	RequireInvalid("paginated relation inherited an unscoped response ceiling", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4));
		ConnectorCatalogTestAccess::Relation("missing_response_scope", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::UnpaginatedResources(5, 64));
	});
	RequireInvalid("paginated relation accepted a page size above its decoder ceiling", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4));
		ConnectorCatalogTestAccess::Relation("undersized_page_records", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 4, 16, 64));
	});
	RequireInvalid("paginated relation accepted records beyond its page sequence", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4));
		ConnectorCatalogTestAccess::Relation("widened_scan_records", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 21, 64));
	});
	RequireInvalid("paginated relation accepted response bytes beyond its page sequence", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4));
		ConnectorCatalogTestAccess::Relation("widened_scan_response", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 4097, 5, 20, 64));
	});
	RequireInvalid("paginated relation accepted inconsistent response byte scopes", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 4));
		ConnectorCatalogTestAccess::Relation("inconsistent_response_scope", Columns(), std::move(operation),
		                                     ConnectorCatalogTestAccess::RequiredBearer(),
		                                     ConnectorCatalogTestAccess::PaginatedResources(1024, 512, 5, 20, 64));
	});
	RequireInvalid("paginated relation accepted an overflowing page resource envelope", []() {
		auto operation =
		    BuildPaginatedOperation(ConnectorCatalogTestAccess::SequentialLink("page_size", 5, "page", 1, 1, 2));
		ConnectorCatalogTestAccess::Relation(
		    "overflowing_page_resources", Columns(), std::move(operation), ConnectorCatalogTestAccess::RequiredBearer(),
		    ConnectorCatalogTestAccess::PaginatedResources(std::numeric_limits<std::uint64_t>::max(),
		                                                   std::numeric_limits<std::uint64_t>::max(), 5, 10, 64));
	});
	RequireInvalid("catalog accepted a relation response ceiling wider than connector policy", []() {
		const auto paginated = BuildValidPaginatedCatalog();
		auto policy = paginated.NetworkPolicy();
		policy.max_response_bytes = 512;
		ConnectorCatalogTestAccess::Catalog(paginated.ConnectorName(), paginated.Version(), paginated.Relations(),
		                                    std::move(policy));
	});
}

} // namespace

namespace cuac_test {

void RunConnectorPaginationContractTests() {
	TestClosedValuesAndExplanation();
	TestPaginationProfileValidation();
	TestScopedResourceValidation();
}

} // namespace cuac_test
