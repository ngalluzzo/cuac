# Runtime consumes only the immutable plan value; planner construction retains
# Connector and Query dependencies behind a separate Semantics service.
set(RELATIONAL_PREDICATE_SOURCES
    src/semantics/predicate/relational_predicate.cpp)
set(RELATIONAL_PLAN_VALUE_SOURCES
    src/semantics/plan/cache_policy.cpp
    src/semantics/plan/planned_graphql_generator_recipe.cpp
    src/semantics/plan/planned_protocol_operation.cpp
    src/semantics/plan/scan_plan.cpp
    src/semantics/plan/scan_plan_explain.cpp)
set(RELATIONAL_PLANNER_SOURCES
    src/semantics/planner/graphql_generator_recipe_planner.cpp
    src/semantics/planner/package_operation_contract.cpp
    src/semantics/planner/graphql_operation_planner.cpp
    src/semantics/planner/input_resolution.cpp
    src/semantics/planner/operation_selection.cpp
    src/semantics/predicate/predicate_classifier.cpp
    src/semantics/planner/rest_operation_planner.cpp
    src/semantics/planner/scan_planner.cpp
    src/semantics/planner/scan_planner_validation.cpp)
set(RELATIONAL_PACKAGE_BOUND_PLANNER_SOURCES
    src/semantics/planner/package_bound_scan_planner.cpp)
set(RELATIONAL_PLANNING_SOURCES
    ${RELATIONAL_PREDICATE_SOURCES}
    ${RELATIONAL_PLAN_VALUE_SOURCES}
    ${RELATIONAL_PLANNER_SOURCES}
    ${RELATIONAL_PACKAGE_BOUND_PLANNER_SOURCES})
