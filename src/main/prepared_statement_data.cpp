#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"

namespace duckdb {

PreparedStatementData::PreparedStatementData(StatementType type)
    : statement_type(type), read_only(true), requires_valid_transaction(true), allow_stream_result(false) {
}

PreparedStatementData::~PreparedStatementData() {
}

void PreparedStatementData::Bind(vector<Value> values) {
	// set parameters
	if (values.size() != value_map.size()) {
		throw BinderException("Parameter/argument count mismatch for prepared statement. Expected %llu, got %llu",
		                      value_map.size(), values.size());
	}
	// bind the values
	for (idx_t i = 0; i < values.size(); i++) {
		auto it = value_map.find(i + 1);
		if (it == value_map.end()) {
			throw BinderException("Could not find parameter with index %llu", i + 1);
		}
		if (it->second.empty()) {
			throw BinderException("No value found for parameter with index %llu", i + 1);
		}
		if (!values[i].TryCastAs(it->second[0]->type())) {
			throw BinderException(
			    "Type mismatch for binding parameter with index %llu, expected type %s but got type %s", i + 1,
			    values[i].type().ToString().c_str(), it->second[0]->type().ToString().c_str());
		}
		for (auto &target : it->second) {
			*target = values[i];
		}
	}
}

LogicalType PreparedStatementData::GetType(idx_t param_idx) {
	auto it = value_map.find(param_idx);
	if (it == value_map.end()) {
		throw BinderException("Could not find parameter with index %llu", param_idx);
	}
	if (it->second.empty()) {
		throw BinderException("No value found for parameter with index %llu", param_idx);
	}
	return it->second[0]->type();
}

void PreparedStatementData::ResetResultValueEntry(idx_t val_idx, const Value &value) {
	D_ASSERT(val_idx > 0);
	auto it = value_map.find(val_idx);
	if (it == value_map.end()) {
		throw BinderException("Could not find parameter with index %llu", val_idx);
	}
	if (it->second.empty()) {
		throw BinderException("No value found for parameter with index %llu", val_idx);
	}
	for (auto &target : it->second) {
		*target = value;
	}

	// statement types
	if (!types.empty() && types.size() <= val_idx) {
		types[val_idx - 1] = value.type();
	}

	// changing the physical plan
	if (!plan->types.empty() && plan->types.size() <= val_idx) {
		plan->types[val_idx - 1] = value.type();
	}

	// changing projection expression
	if (plan->type == PhysicalOperatorType::PROJECTION) {
		auto proj_plan = (PhysicalProjection *)plan.get();
		if (!proj_plan->select_list.empty() && proj_plan->select_list.size() <= val_idx) {
			proj_plan->select_list[val_idx - 1]->return_type = value.type();
		}
	}
}

} // namespace duckdb
