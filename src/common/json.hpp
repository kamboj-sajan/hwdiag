// A small JSON value type and writer. The tools only ever write JSON (the
// Python harness does the reading), so there is no parser.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hwdiag::json {

class Value {
 public:
  enum class Type { Null, Bool, Int, UInt, Double, String, Array, Object };

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : type_(Type::Bool), b_(b) {}
  Value(int v) : type_(Type::Int), i_(v) {}
  Value(long v) : type_(Type::Int), i_(v) {}
  Value(long long v) : type_(Type::Int), i_(v) {}
  Value(unsigned v) : type_(Type::UInt), u_(v) {}
  Value(unsigned long v) : type_(Type::UInt), u_(v) {}
  Value(unsigned long long v) : type_(Type::UInt), u_(v) {}
  Value(double v) : type_(Type::Double), d_(v) {}
  Value(const char* s) : type_(Type::String), s_(s) {}
  Value(std::string s) : type_(Type::String), s_(std::move(s)) {}
  Value(std::string_view s) : type_(Type::String), s_(s) {}

  static Value object();
  static Value array();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }

  // Object access: inserts a null member if the key is new. Keys keep
  // insertion order, so output is stable and readable.
  Value& operator[](std::string_view key);
  const Value* find(std::string_view key) const;

  // Array append. Turns a null value into an array first.
  void push_back(Value v);
  size_t size() const;

  // indent < 0 writes compact JSON; otherwise pretty-prints with that many spaces.
  std::string dump(int indent = -1) const;

 private:
  void write(std::string& out, int indent, int depth) const;

  Type type_ = Type::Null;
  bool b_ = false;
  int64_t i_ = 0;
  uint64_t u_ = 0;
  double d_ = 0.0;
  std::string s_;
  std::vector<Value> items_;      // array elements, or object values
  std::vector<std::string> keys_; // object keys, parallel to items_
};

// Escapes a string for JSON and wraps it in quotes. Control characters are
// escaped, and bytes that are not valid UTF-8 become U+FFFD.
std::string quote(std::string_view s);

// Shortest round-trip form of a double; non-finite values become null.
std::string format_number(double d);

}  // namespace hwdiag::json
