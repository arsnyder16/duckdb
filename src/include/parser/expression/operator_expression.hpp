//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parser/expression/operator_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "parser/expression.hpp"
#include "parser/sql_node_visitor.hpp"

namespace duckdb {
//! Represents a built-in operator expression
class OperatorExpression : public Expression {
public:
	OperatorExpression(ExpressionType type, TypeId type_id = TypeId::INVALID) : Expression(type, type_id) {
	}
	OperatorExpression(ExpressionType type, TypeId type_id, unique_ptr<Expression> left,
	                   unique_ptr<Expression> right = nullptr)
	   : Expression(type, type_id) {
		this->left = move(left);
		this->right = move(right);
	}

	void ResolveType() override;

	unique_ptr<Expression> Accept(SQLNodeVisitor *v) override {
		return v->Visit(*this);
	}
	ExpressionClass GetExpressionClass() override {
		return ExpressionClass::OPERATOR;
	}

	unique_ptr<Expression> Copy() override;

	void EnumerateChildren(std::function<unique_ptr<Expression>(unique_ptr<Expression> expression)> callback) override;
	void EnumerateChildren(std::function<void(Expression* expression)> callback) const override;
	
	//! Serializes a OperatorExpression to a stand-alone binary blob
	void Serialize(Serializer &serializer) override;
	//! Deserializes a blob back into an OperatorExpression
	static unique_ptr<Expression> Deserialize(ExpressionType type, TypeId return_type, Deserializer &source);
	string ToString() const override {
		return left->ToString() + ExpressionTypeToOperator(type) + right->ToString();
	}

	unique_ptr<Expression> left;
	unique_ptr<Expression> right;
};
} // namespace duckdb
