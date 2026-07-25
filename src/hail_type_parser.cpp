#include "hail_type_parser.hpp"

#include <cctype>
#include <stdexcept>

namespace duckdb {

namespace {

// Minimal recursive-descent cursor over a type string. Throws std::runtime_error
// with the offending string on any malformed input — bind-time errors surface
// to the user as a BinderException (Task 3 wraps these).
class TypeStringCursor {
public:
	explicit TypeStringCursor(const std::string &s) : s_(s), pos_(0) {
	}

	bool eof() const {
		return pos_ >= s_.size();
	}

	char peek() const {
		if (eof()) {
			throw std::runtime_error("Unexpected end of type string: " + s_);
		}
		return s_[pos_];
	}

	void advance() {
		pos_++;
	}

	void expect(char c) {
		if (eof() || s_[pos_] != c) {
			throw std::runtime_error("Expected '" + std::string(1, c) + "' at position " +
			                          std::to_string(pos_) + " in type string: " + s_);
		}
		pos_++;
	}

	bool try_consume_literal(const std::string &lit) {
		if (s_.compare(pos_, lit.size(), lit) == 0) {
			pos_ += lit.size();
			return true;
		}
		return false;
	}

	std::string read_identifier() {
		size_t start = pos_;
		while (!eof() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) {
			pos_++;
		}
		if (pos_ == start) {
			throw std::runtime_error("Expected identifier at position " + std::to_string(start) +
			                          " in type string: " + s_);
		}
		return s_.substr(start, pos_ - start);
	}

	void expect_eof() {
		if (!eof()) {
			throw std::runtime_error("Trailing characters at position " + std::to_string(pos_) +
			                          " in type string: " + s_);
		}
	}

private:
	const std::string &s_;
	size_t pos_;
};

ETypeNode parse_etype_inner(TypeStringCursor &cur) {
	ETypeNode node;
	// A leading '+' means "required" wherever an etype occurs — as a struct
	// field's value, an array's element type, or the top-level type itself
	// (confirmed against real Hail output: "+EBaseStruct{...}" at the top
	// level, not just on nested fields/elements — see the Grammar section).
	if (!cur.eof() && cur.peek() == '+') {
		cur.advance();
		node.required = true;
	}
	if (cur.try_consume_literal("EInt32")) {
		node.kind = EKind::Int32;
	} else if (cur.try_consume_literal("EInt64")) {
		node.kind = EKind::Int64;
	} else if (cur.try_consume_literal("EFloat32")) {
		node.kind = EKind::Float32;
	} else if (cur.try_consume_literal("EFloat64")) {
		node.kind = EKind::Float64;
	} else if (cur.try_consume_literal("EBoolean")) {
		node.kind = EKind::Boolean;
	} else if (cur.try_consume_literal("EBinary")) {
		node.kind = EKind::Binary;
	} else if (cur.try_consume_literal("EArray")) {
		node.kind = EKind::Array;
		cur.expect('[');
		node.children.push_back(parse_etype_inner(cur)); // element consumes its own leading '+', if any
		cur.expect(']');
	} else if (cur.try_consume_literal("EBaseStruct")) {
		node.kind = EKind::BaseStruct;
		cur.expect('{');
		if (!cur.eof() && cur.peek() != '}') {
			while (true) {
				std::string field_name = cur.read_identifier();
				cur.expect(':');
				ETypeNode field_value = parse_etype_inner(cur); // consumes its own leading '+', if any
				field_value.name = field_name;
				node.children.push_back(std::move(field_value));
				if (!cur.eof() && cur.peek() == ',') {
					cur.advance();
					continue;
				}
				break;
			}
		}
		cur.expect('}');
	} else {
		throw std::runtime_error("Unrecognized EType in type string");
	}
	return node;
}

VTypeNode parse_vtype_inner(TypeStringCursor &cur) {
	VTypeNode node;
	if (cur.try_consume_literal("Int32")) {
		node.kind = VKind::Int32;
	} else if (cur.try_consume_literal("Int64")) {
		node.kind = VKind::Int64;
	} else if (cur.try_consume_literal("Float32")) {
		node.kind = VKind::Float32;
	} else if (cur.try_consume_literal("Float64")) {
		node.kind = VKind::Float64;
	} else if (cur.try_consume_literal("Boolean")) {
		node.kind = VKind::Boolean;
	} else if (cur.try_consume_literal("String")) {
		node.kind = VKind::String;
	} else if (cur.try_consume_literal("Locus")) {
		node.kind = VKind::Locus;
		cur.expect('(');
		node.genome = cur.read_identifier();
		cur.expect(')');
	} else if (cur.try_consume_literal("Array")) {
		node.kind = VKind::Array;
		cur.expect('[');
		node.children.push_back(parse_vtype_inner(cur));
		cur.expect(']');
	} else if (cur.try_consume_literal("Struct")) {
		node.kind = VKind::Struct;
		cur.expect('{');
		if (!cur.eof() && cur.peek() != '}') {
			while (true) {
				std::string field_name = cur.read_identifier();
				cur.expect(':');
				VTypeNode field_value = parse_vtype_inner(cur);
				field_value.name = field_name;
				node.children.push_back(std::move(field_value));
				if (!cur.eof() && cur.peek() == ',') {
					cur.advance();
					continue;
				}
				break;
			}
		}
		cur.expect('}');
	} else {
		throw std::runtime_error("Unrecognized VType in type string");
	}
	return node;
}

} // namespace

ETypeNode parse_etype(const std::string &s) {
	TypeStringCursor cur(s);
	ETypeNode node = parse_etype_inner(cur); // let expect()/read_identifier() exceptions propagate as-is
	cur.expect_eof();
	return node;
}

VTypeNode parse_vtype(const std::string &s) {
	TypeStringCursor cur(s);
	VTypeNode node = parse_vtype_inner(cur);
	cur.expect_eof();
	return node;
}

std::string etype_to_string(const ETypeNode &node) {
	std::string out;
	// Emit '+' for this node's own required flag (applies at all nesting levels,
	// including the top-level node)
	if (node.required) {
		out += "+";
	}
	switch (node.kind) {
	case EKind::Int32:
		out += "EInt32";
		break;
	case EKind::Int64:
		out += "EInt64";
		break;
	case EKind::Float32:
		out += "EFloat32";
		break;
	case EKind::Float64:
		out += "EFloat64";
		break;
	case EKind::Boolean:
		out += "EBoolean";
		break;
	case EKind::Binary:
		out += "EBinary";
		break;
	case EKind::Array: {
		out += "EArray[";
		// Recursively serialize the element; it will emit its own '+' if required
		out += etype_to_string(node.children[0]);
		out += "]";
		break;
	}
	case EKind::BaseStruct: {
		out += "EBaseStruct{";
		for (size_t i = 0; i < node.children.size(); i++) {
			if (i > 0) {
				out += ",";
			}
			auto &f = node.children[i];
			out += f.name + ":";
			// Recursively serialize the field; it will emit its own '+' if required
			out += etype_to_string(f);
		}
		out += "}";
		break;
	}
	default:
		throw std::runtime_error("Unhandled EKind in etype_to_string");
	}
	return out;
}

std::string vtype_to_string(const VTypeNode &node) {
	switch (node.kind) {
	case VKind::Int32:
		return "Int32";
	case VKind::Int64:
		return "Int64";
	case VKind::Float32:
		return "Float32";
	case VKind::Float64:
		return "Float64";
	case VKind::Boolean:
		return "Boolean";
	case VKind::String:
		return "String";
	case VKind::Locus:
		return "Locus(" + node.genome + ")";
	case VKind::Array:
		return "Array[" + vtype_to_string(node.children[0]) + "]";
	case VKind::Struct: {
		std::string out = "Struct{";
		for (size_t i = 0; i < node.children.size(); i++) {
			if (i > 0) {
				out += ",";
			}
			out += node.children[i].name + ":" + vtype_to_string(node.children[i]);
		}
		out += "}";
		return out;
	}
	}
	throw std::runtime_error("Unhandled VKind in vtype_to_string");
}

LogicalType VTypeToDuckDBType(const VTypeNode &vtype) {
	switch (vtype.kind) {
	case VKind::Int32:
		return LogicalType::INTEGER;
	case VKind::Int64:
		return LogicalType::BIGINT;
	case VKind::Float32:
		return LogicalType::FLOAT;
	case VKind::Float64:
		return LogicalType::DOUBLE;
	case VKind::Boolean:
		return LogicalType::BOOLEAN;
	case VKind::String:
		return LogicalType::VARCHAR;
	case VKind::Locus:
		return LogicalType::STRUCT(
		    {{"contig", LogicalType::VARCHAR}, {"position", LogicalType::INTEGER}});
	case VKind::Array:
		return LogicalType::LIST(VTypeToDuckDBType(vtype.children[0]));
	case VKind::Struct: {
		child_list_t<LogicalType> children;
		for (auto &f : vtype.children) {
			children.emplace_back(f.name, VTypeToDuckDBType(f));
		}
		return LogicalType::STRUCT(children);
	}
	}
	throw std::runtime_error("Unhandled VKind in VTypeToDuckDBType");
}

} // namespace duckdb
