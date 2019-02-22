//===----------------------------------------------------------------------===//
//                         DuckDB
//
// execution/operator/join/physical_delim_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/types/chunk_collection.hpp"
#include "execution/physical_operator.hpp"

namespace duckdb {
//! PhysicalDelimJoin represents a join where the LHS will be duplicate eliminated and pushed into a
//! PhysicalChunkCollectionScan in the RHS.
class PhysicalDelimJoin : public PhysicalOperator {
public:
	PhysicalDelimJoin(LogicalOperator &op, unique_ptr<PhysicalOperator> original_join,
	                  vector<PhysicalOperator *> delim_scans);

	void _GetChunk(ClientContext &context, DataChunk &chunk, PhysicalOperatorState *state) override;
	unique_ptr<PhysicalOperatorState> GetOperatorState(ExpressionExecutor *parent_executor) override;
	string ExtraRenderInformation() override;

	unique_ptr<PhysicalOperator> join;
	unique_ptr<PhysicalOperator> distinct;
	ChunkCollection lhs_data;
	ChunkCollection delim_data;
};

class PhysicalDelimJoinState : public PhysicalOperatorState {
public:
	PhysicalDelimJoinState(PhysicalOperator *left, ExpressionExecutor *parent_executor)
	    : PhysicalOperatorState(left, parent_executor) {
	}

	unique_ptr<PhysicalOperatorState> join_state;
};
} // namespace duckdb
